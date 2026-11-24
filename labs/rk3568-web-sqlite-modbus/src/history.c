/* SQLite v1历史库：独立写线程、64项队列、短事务、FULL同步。
 * 仅在显式--db指定路径时启用；不删除历史、不把历史作为实时快照。
 */
#define _POSIX_C_SOURCE 200809L
#include "history.h"
#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#define CAPACITY 64
#define APP_ID 1412771889
struct record { uint16_t words[64]; int changed; };
static struct record queue[CAPACITY];
static unsigned head,tail,count,interval_s=10;
static bool enabled,started,stopping;
static atomic_bool degraded,lost;
static atomic_uint dropped;
static void mark_lost(void)
{
    atomic_store(&lost,true);
    unsigned value=atomic_load(&dropped);
    while (value!=UINT_MAX && !atomic_compare_exchange_weak(&dropped,&value,value+1)) {}
}
static sqlite3 *db;
static pthread_t thread;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready=PTHREAD_COND_INITIALIZER;
static uint16_t previous[64];
static bool have_previous;
static uint64_t last_saved;

static uint64_t wide(const uint16_t *p)
{
    return ((uint64_t)p[0]<<48)|((uint64_t)p[1]<<32)|((uint64_t)p[2]<<16)|p[3];
}
static int exec_sql(const char *sql)
{
    char *message=NULL; int rc=sqlite3_exec(db,sql,NULL,NULL,&message);
    if (rc!=SQLITE_OK) { fprintf(stderr,"HISTORY_SQL rc=%d %s\n",rc,message?message:sqlite3_errmsg(db));sqlite3_free(message);return -1; }
    return 0;
}
static int scalar(const char *sql,int *value)
{
    sqlite3_stmt *s=NULL;
    int rc=sqlite3_prepare_v2(db,sql,-1,&s,NULL);
    if (rc==SQLITE_OK && sqlite3_step(s)==SQLITE_ROW) { *value=sqlite3_column_int(s,0);rc=SQLITE_OK; }
    else rc=SQLITE_ERROR;
    sqlite3_finalize(s); return rc==SQLITE_OK?0:-1;
}
static int write_record(const struct record *r)
{
    const uint16_t *s=r->words; char session[17],ip[16]; unsigned char raw[128];
    sqlite3_stmt *stmt=NULL; int result=-1;
    snprintf(session,sizeof(session),"%016llx",(unsigned long long)wide(s+4));
    snprintf(ip,sizeof(ip),"%u.%u.%u.%u",s[40]>>8,s[40]&255,s[41]>>8,s[41]&255);
    for (int i=0;i<64;i++) { raw[2*i]=(unsigned char)(s[i]>>8);raw[2*i+1]=(unsigned char)s[i]; }
    if (exec_sql("BEGIN IMMEDIATE")) return -1;
    if (sqlite3_prepare_v2(db,"INSERT OR IGNORE INTO sessions(session,started_ms) VALUES(?,?)",-1,&stmt,NULL)!=SQLITE_OK) goto done;
    sqlite3_bind_text(stmt,1,session,-1,SQLITE_TRANSIENT);sqlite3_bind_int64(stmt,2,(sqlite3_int64)wide(s+12));
    if (sqlite3_step(stmt)!=SQLITE_DONE) goto done;
    sqlite3_finalize(stmt);stmt=NULL;
    if (sqlite3_prepare_v2(db,"INSERT INTO samples(session,seq,mono_ms,wall_ms,valid,carrier,ipv4,signal_dbm,registers) VALUES(?,?,?,?,?,?,?,?,?)",-1,&stmt,NULL)!=SQLITE_OK) goto done;
    sqlite3_bind_text(stmt,1,session,-1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt,2,((uint32_t)s[2]<<16)|s[3]);
    sqlite3_bind_int64(stmt,3,(sqlite3_int64)wide(s+8));sqlite3_bind_int64(stmt,4,(sqlite3_int64)wide(s+12));
    sqlite3_bind_int(stmt,5,s[1]);
    if (s[1]&1) sqlite3_bind_int(stmt,6,s[18]); else sqlite3_bind_null(stmt,6);
    if (s[1]&2) sqlite3_bind_text(stmt,7,ip,-1,SQLITE_TRANSIENT); else sqlite3_bind_null(stmt,7);
    if (s[1]&4) sqlite3_bind_int(stmt,8,(int16_t)s[42]); else sqlite3_bind_null(stmt,8);
    sqlite3_bind_blob(stmt,9,raw,128,SQLITE_TRANSIENT);
    if (sqlite3_step(stmt)!=SQLITE_DONE) goto done;
    sqlite3_finalize(stmt);stmt=NULL;
    if (r->changed) {
        if (sqlite3_prepare_v2(db,"INSERT INTO events(session,seq,wall_ms,kind) VALUES(?,?,?,?)",-1,&stmt,NULL)!=SQLITE_OK) goto done;
        sqlite3_bind_text(stmt,1,session,-1,SQLITE_TRANSIENT);sqlite3_bind_int64(stmt,2,((uint32_t)s[2]<<16)|s[3]);
        sqlite3_bind_int64(stmt,3,(sqlite3_int64)wide(s+12));
        sqlite3_bind_text(stmt,4,r->changed==2?"session_start":"state_change",-1,SQLITE_STATIC);
        if (sqlite3_step(stmt)!=SQLITE_DONE) goto done;
    }
    sqlite3_finalize(stmt);stmt=NULL;
    if (exec_sql("COMMIT")==0) result=0;
done:
    sqlite3_finalize(stmt);
    if (result) { fprintf(stderr,"HISTORY_WRITE %s\n",sqlite3_errmsg(db));(void)exec_sql("ROLLBACK"); }
    return result;
}
static void *writer(void *unused)
{
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&mutex);
        while (!count && !stopping) pthread_cond_wait(&ready,&mutex);
        if (!count && stopping) { pthread_mutex_unlock(&mutex);break; }
        struct record r=queue[head];head=(head+1)%CAPACITY;--count;
        pthread_mutex_unlock(&mutex);
        if (write_record(&r)) { atomic_store(&degraded,true);mark_lost(); }
        else atomic_store(&degraded,false);
    }
    (void)sqlite3_wal_checkpoint_v2(db,NULL,SQLITE_CHECKPOINT_PASSIVE,NULL,NULL);
    if (sqlite3_close(db)!=SQLITE_OK) mark_lost();
    db=NULL;return NULL;
}


int history_start(const char *path,unsigned interval_seconds)
{
    if (!path) return 0;
    enabled=true;interval_s=interval_seconds;
    if (sqlite3_open_v2(path,&db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,NULL)!=SQLITE_OK) goto fail;
    sqlite3_busy_timeout(db,250);
    int version=0,application=0,tables=0;
    if (scalar("PRAGMA user_version",&version) || scalar("PRAGMA application_id",&application)) goto fail;
    if (version==0 && application==0) {
        if (scalar("SELECT count(*) FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'",&tables) || tables) goto fail;
        if (exec_sql("BEGIN IMMEDIATE;"
          "CREATE TABLE sessions(session TEXT PRIMARY KEY,started_ms INTEGER NOT NULL);"
          "CREATE TABLE samples(id INTEGER PRIMARY KEY,session TEXT NOT NULL,seq INTEGER NOT NULL,mono_ms INTEGER NOT NULL,wall_ms INTEGER NOT NULL,valid INTEGER NOT NULL,carrier INTEGER,ipv4 TEXT,signal_dbm INTEGER,registers BLOB NOT NULL CHECK(length(registers)=128));"
          "CREATE INDEX samples_session_time ON samples(session,mono_ms,id);"
          "CREATE TABLE events(id INTEGER PRIMARY KEY,session TEXT NOT NULL,seq INTEGER NOT NULL,wall_ms INTEGER NOT NULL,kind TEXT NOT NULL);"
          "CREATE INDEX events_session_time ON events(session,wall_ms,id);"
          "PRAGMA application_id=1412771889;PRAGMA user_version=1;COMMIT;")) goto fail;
    } else if (version!=1 || application!=APP_ID) { fprintf(stderr,"HISTORY_SCHEMA_UNSUPPORTED\n");goto fail; }
    sqlite3_stmt *mode=NULL;
    if (sqlite3_prepare_v2(db,"PRAGMA journal_mode=WAL",-1,&mode,NULL)!=SQLITE_OK) goto fail;
    int row=sqlite3_step(mode);
    bool wal=row==SQLITE_ROW && sqlite3_column_text(mode,0) && strcmp((const char*)sqlite3_column_text(mode,0),"wal")==0;
    sqlite3_finalize(mode);
    if (!wal || exec_sql("PRAGMA synchronous=FULL;PRAGMA wal_autocheckpoint=64;PRAGMA journal_size_limit=1048576;PRAGMA max_page_count=16384;")) goto fail;
    if (pthread_create(&thread,NULL,writer,NULL)) goto fail;
    started=true;fprintf(stderr,"HISTORY_READY schema=1 interval=%u journal=WAL synchronous=FULL\n",interval_s);return 0;
fail:
    fprintf(stderr,"HISTORY_DISABLED %s\n",db?sqlite3_errmsg(db):"open failed");
    if (db) { sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);sqlite3_close(db);db=NULL; }
    atomic_store(&degraded,true);mark_lost();return -1;
}
void history_offer(const uint16_t s[64])
{
    if (!started) return;
    uint64_t now=wide(s+8);
    int changed=!have_previous?2:(((s[1]^previous[1])&7) || s[18]!=previous[18] ||
          s[40]!=previous[40] || s[41]!=previous[41] || s[46]!=previous[46] || s[47]!=previous[47]);
    if (!changed && now-last_saved<(uint64_t)interval_s*1000) return;
    pthread_mutex_lock(&mutex);
    if (count==CAPACITY) { mark_lost();pthread_mutex_unlock(&mutex);return; }
    memcpy(queue[tail].words,s,128);queue[tail].changed=changed;tail=(tail+1)%CAPACITY;++count;
    pthread_cond_signal(&ready);pthread_mutex_unlock(&mutex);
    memcpy(previous,s,128);have_previous=true;last_saved=now;
}
unsigned history_diagnostics(void)
{
    return (enabled && atomic_load(&degraded)?2U:0U)|(atomic_load(&lost)?4U:0U);
}
int history_finish(void)
{
    if (started) {
        pthread_mutex_lock(&mutex);stopping=true;pthread_cond_signal(&ready);pthread_mutex_unlock(&mutex);
        pthread_join(thread,NULL);started=false;
    }
    if (enabled) fprintf(stderr,"HISTORY_STOP dropped_or_errors=%u\n",atomic_load(&dropped));
    return atomic_load(&lost)?-1:0;
}
