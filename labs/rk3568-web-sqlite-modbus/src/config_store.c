/* 独立SQLite配置库：版本门禁、短事务CAS更新、最多1000条审计。
 * 运行事实采用私有目录内原子rename和单调时间有效期，不把已保存值当成实际值。 */
#define _GNU_SOURCE
#include "config_store.h"
#include <sqlite3.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define CONFIG_APP_ID 1412771890
static int scalar(sqlite3 *db,const char *sql)
{
    sqlite3_stmt *s=NULL;int v=-1;
    if(sqlite3_prepare_v2(db,sql,-1,&s,NULL)==SQLITE_OK&&sqlite3_step(s)==SQLITE_ROW)v=sqlite3_column_int(s,0);
    sqlite3_finalize(s);return v;
}
static int schema(sqlite3 *db) {return scalar(db,"PRAGMA application_id")==CONFIG_APP_ID&&scalar(db,"PRAGMA user_version")==1;}
static int sql(sqlite3 *db,const char *text) {return sqlite3_exec(db,text,NULL,NULL,NULL)==SQLITE_OK?0:-1;}
int config_valid(const struct task_config *c)
{
    const int baud[]={1200,2400,4800,9600,19200,38400,57600,115200};int found=0;
    for(unsigned i=0;i<sizeof(baud)/sizeof(baud[0]);i++)if(c->baud==baud[i])found=1;
    return c->revision>=1&&c->revision<INT_MAX&&c->unit>=1&&c->unit<=247&&found&&
        c->data_bits==8&&(c->stop_bits==1||c->stop_bits==2)&&strchr("NEO",c->parity)&&c->parity&&
        c->history_seconds>=1&&c->history_seconds<=3600;
}
int config_load(const char *path,struct task_config *c)
{
    sqlite3 *db=NULL;sqlite3_stmt *s=NULL;int result=-1;
    if(sqlite3_open_v2(path,&db,SQLITE_OPEN_READONLY,NULL)!=SQLITE_OK)goto done;
    sqlite3_busy_timeout(db,200);
    if(!schema(db)||sqlite3_prepare_v2(db,"SELECT revision,unit,baud,data_bits,stop_bits,history_seconds,parity FROM settings WHERE id=1",-1,&s,NULL)!=SQLITE_OK)goto done;
    if(sqlite3_step(s)!=SQLITE_ROW)goto done;
    for(int i=0;i<6;i++)if(sqlite3_column_type(s,i)!=SQLITE_INTEGER||sqlite3_column_int64(s,i)>INT_MAX||sqlite3_column_int64(s,i)<1)goto done;
    if(sqlite3_column_type(s,6)!=SQLITE_TEXT||sqlite3_column_bytes(s,6)!=1)goto done;
    *c=(struct task_config){sqlite3_column_int(s,0),sqlite3_column_int(s,1),sqlite3_column_int(s,2),sqlite3_column_int(s,3),sqlite3_column_int(s,4),sqlite3_column_int(s,5),(char)sqlite3_column_text(s,6)[0]};
    if(config_valid(c))result=0;
done:
    sqlite3_finalize(s);if(db)sqlite3_close(db);return result;
}
int config_init(const char *path)
{
    sqlite3 *db=NULL;int result=-1;
    if(sqlite3_open_v2(path,&db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,NULL)!=SQLITE_OK)goto done;
    sqlite3_busy_timeout(db,200);
    if(schema(db)){result=0;goto done;}
    if(scalar(db,"PRAGMA application_id")!=0||scalar(db,"PRAGMA user_version")!=0||scalar(db,"SELECT count(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'")!=0)goto done;
    if(sql(db,"PRAGMA synchronous=FULL; BEGIN IMMEDIATE;"))goto done;
    if(sql(db,"CREATE TABLE settings(id INTEGER PRIMARY KEY CHECK(id=1),revision INTEGER NOT NULL,unit INTEGER NOT NULL,baud INTEGER NOT NULL,data_bits INTEGER NOT NULL,stop_bits INTEGER NOT NULL,history_seconds INTEGER NOT NULL,parity TEXT NOT NULL);"
        "INSERT INTO settings VALUES(1,1,1,9600,8,1,10,'N');"
        "CREATE TABLE audit(revision INTEGER PRIMARY KEY,unit INTEGER NOT NULL,baud INTEGER NOT NULL,parity TEXT NOT NULL,stop_bits INTEGER NOT NULL,history_seconds INTEGER NOT NULL);"
        "PRAGMA application_id=1412771890; PRAGMA user_version=1; COMMIT;")){sql(db,"ROLLBACK");goto done;}
    result=0;
done:
    if(db)sqlite3_close(db);
    if(!result){struct task_config c;result=config_load(path,&c);}return result;
}
int config_save(const char *path,int expected,struct task_config *c)
{
    if(expected<1||expected>=INT_MAX-1)return 400;
    c->revision=expected+1;if(!config_valid(c))return 400;
    sqlite3 *db=NULL;sqlite3_stmt *s=NULL;int result=503;char parity[2]={c->parity,0};
    if(sqlite3_open_v2(path,&db,SQLITE_OPEN_READWRITE,NULL)!=SQLITE_OK)goto done;
    sqlite3_busy_timeout(db,200);
    if(!schema(db)||sql(db,"PRAGMA synchronous=FULL; BEGIN IMMEDIATE;"))goto done;
    if(sqlite3_prepare_v2(db,"UPDATE settings SET revision=?,unit=?,baud=?,data_bits=?,stop_bits=?,history_seconds=?,parity=? WHERE id=1 AND revision=?",-1,&s,NULL)!=SQLITE_OK)goto rollback;
    sqlite3_bind_int(s,1,c->revision);sqlite3_bind_int(s,2,c->unit);sqlite3_bind_int(s,3,c->baud);sqlite3_bind_int(s,4,c->data_bits);sqlite3_bind_int(s,5,c->stop_bits);sqlite3_bind_int(s,6,c->history_seconds);sqlite3_bind_text(s,7,parity,-1,SQLITE_TRANSIENT);sqlite3_bind_int(s,8,expected);
    if(sqlite3_step(s)!=SQLITE_DONE)goto rollback;
    if(sqlite3_changes(db)!=1){result=409;goto rollback;}
    sqlite3_finalize(s);s=NULL;
    if(sqlite3_prepare_v2(db,"INSERT INTO audit SELECT revision,unit,baud,parity,stop_bits,history_seconds FROM settings WHERE id=1",-1,&s,NULL)!=SQLITE_OK||sqlite3_step(s)!=SQLITE_DONE)goto rollback;
    sqlite3_finalize(s);s=NULL;
    if(sql(db,"DELETE FROM audit WHERE revision < (SELECT max(revision)-999 FROM audit); COMMIT;"))goto rollback;
    result=200;goto done;
rollback:sql(db,"ROLLBACK");
done:sqlite3_finalize(s);if(db)sqlite3_close(db);return result;
}
/* 只认新路径，避免覆盖其他任务或旧进程的运行事实。 */
int runtime_claim(const char *path)
{
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0)return -1;
    return close(fd);
}
int runtime_write(const char *path,const struct task_runtime *r)
{
    char tmp[PATH_MAX],text[256];
    int n=snprintf(tmp,sizeof(tmp),"%s.tmp.%ld",path,(long)getpid());if(n<0||(size_t)n>=sizeof(tmp))return -1;
    int fd=open(tmp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return -1;
    n=snprintf(text,sizeof(text),"v1 %d %d %d %d %llu %llu\n",r->revision,r->unit,r->history_seconds,r->history_enabled,(unsigned long long)r->mono_ms,(unsigned long long)r->session);
    int result=write(fd,text,(size_t)n)==n?0:-1;
    if(close(fd))result=-1;
    if(!result&&rename(tmp,path))result=-1;
    if(result)unlink(tmp);
    return result;
}
int runtime_read(const char *path,struct task_runtime *r)
{
    if(!path)return -1;
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);if(fd<0)return -1;
    struct stat st;char text[257],extra;int result=-1;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<1||st.st_size>256)goto done;
    ssize_t n=read(fd,text,256);if(n<1)goto done;text[n]=0;
    unsigned long long mono,session;
    if(sscanf(text,"v1 %d %d %d %d %llu %llu %c",&r->revision,&r->unit,&r->history_seconds,&r->history_enabled,&mono,&session,&extra)!=6)goto done;
    struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))goto done;
    uint64_t now=(uint64_t)t.tv_sec*1000U+t.tv_nsec/1000000U;
    if(r->revision<1||r->unit<1||r->unit>247||r->history_seconds<1||r->history_seconds>3600||(r->history_enabled!=0&&r->history_enabled!=1)||mono>now||now-mono>3000)goto done;
    r->mono_ms=mono;r->session=session;result=0;
done:close(fd);return result;
}
