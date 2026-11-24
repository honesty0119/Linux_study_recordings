/* Web只读API：Unix域SCGI入口，实时读Modbus快照，历史只读SQLite。
 * 与nginx独立进程配合；固定路由、限量游标查询、超时和JSON转义。
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <modbus.h>
#include <poll.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "config_store.h"
static volatile sig_atomic_t stopped;
static const char *database,*socket_path,*target="127.0.0.1";
static int target_port=502;
static const char *runtime_path;
static void halt(int sig) { (void)sig;stopped=1; }
static uint64_t ms(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000; }
static uint64_t wide(const uint16_t *p) { return ((uint64_t)p[0]<<48)|((uint64_t)p[1]<<32)|((uint64_t)p[2]<<16)|p[3]; }
/* 字符串统一转义，避免数据库内容破坏JSON；前端只用textContent。 */
static void quoted(FILE *f,const unsigned char *s)
{
    if (!s) { fputs("null",f);return; }
    fputc('"',f);
    for (;*s;s++) {
        if (*s=='"'||*s=='\\') { fputc('\\',f);fputc(*s,f); }
        else if (*s<32) fprintf(f,"\\u%04x",*s);
        else fputc(*s,f);
    }
    fputc('"',f);
}
static int exact(int fd,void *buffer,size_t length,uint64_t deadline)
{
    unsigned char *p=buffer;size_t n=0;
    while (n<length && !stopped) {
        uint64_t now=ms();if (now>=deadline) return -1;
        struct pollfd pollfd={.fd=fd,.events=POLLIN};int rc=poll(&pollfd,1,(int)(deadline-now));
        if (rc<0&&errno==EINTR) continue;
        if (rc<=0) return -1;
        ssize_t got=recv(fd,p+n,length-n,0);
        if (got<0&&errno==EINTR) continue;
        if (got<=0) return -1;
        n+=(size_t)got;
    }
    return n==length?0:-1;
}
/* 限制为五个显式SCGI参数，拒绝重复参数、请求体与超长头。 */
static int request(int fd,char *method,char *path,char *query)
{
    char prefix[6],headers[4097];size_t digits=0,length=0;uint64_t deadline=ms()+1000;
    for (;;) {
        char c;if (exact(fd,&c,1,deadline)) return 400;
        if (c==':') break;
        if (c<'0'||c>'9'||digits==5) return 400;
        prefix[digits++]=c;length=length*10+(unsigned)(c-'0');
    }
    if (!digits||length<1||length>4096||(digits>1&&prefix[0]=='0')) return 400;
    if (exact(fd,headers,length+1,deadline)||headers[length]!=',') return 400;
    unsigned seen=0;size_t pos=0;
    while (pos<length) {
        char *key=headers+pos;size_t n=strnlen(key,length-pos);if (!n||pos+n>=length) return 400;pos+=n+1;
        char *value=headers+pos;n=strnlen(value,length-pos);if (pos+n>=length) return 400;pos+=n+1;
        unsigned bit;
        if (!strcmp(key,"CONTENT_LENGTH")) {bit=1;if (seen||strcmp(value,"0")) return 400;}
        else if (!strcmp(key,"SCGI")) {bit=2;if (strcmp(value,"1")) return 400;}
        else if (!strcmp(key,"REQUEST_METHOD")) {bit=4;if(n>=16)return 400;strcpy(method,value);}
        else if (!strcmp(key,"PATH_INFO")) {bit=8;if(n>=128)return 400;strcpy(path,value);}
        else if (!strcmp(key,"QUERY_STRING")) {bit=16;if(n>=512)return 400;strcpy(query,value);}
        else return 400;
        if (seen&bit) return 400;
        seen|=bit;
    }
    return seen==31?0:400;
}
static int realtime(FILE *out)
{
    int unit=1;
    if(runtime_path){struct task_runtime r;if(runtime_read(runtime_path,&r))return 503;unit=r.unit;}
    int fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0)return 503;
    struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons((uint16_t)target_port)};
    inet_pton(AF_INET,target,&a.sin_addr);int old=fcntl(fd,F_GETFL);
    if (old<0||fcntl(fd,F_SETFL,old|O_NONBLOCK)<0) {close(fd);return 503;}
    int rc=connect(fd,(struct sockaddr*)&a,sizeof(a));
    if(rc<0) {
        if(errno!=EINPROGRESS) {close(fd);return 503;}
        struct pollfd p={.fd=fd,.events=POLLOUT};int error=0;socklen_t size=sizeof(error);
        if(poll(&p,1,400)<=0||getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&size)||error) {close(fd);return 503;}
    }
    if(fcntl(fd,F_SETFL,old)<0) {close(fd);return 503;}
    modbus_t *ctx=modbus_new_tcp(target,target_port);if(!ctx){close(fd);return 503;}
    uint16_t s[64];modbus_set_socket(ctx,fd);modbus_set_slave(ctx,unit);modbus_set_response_timeout(ctx,0,500000);modbus_set_byte_timeout(ctx,0,100000);
    rc=modbus_read_registers(ctx,0,64,s);modbus_close(ctx);modbus_free(ctx);
    if(rc!=64||s[0]!=0x0200)return 503;
    unsigned age=((unsigned)s[16]<<16)|s[17];
    fprintf(out,"{\"source\":\"live_modbus\",\"session\":\"%016llx\",\"seq\":%u,\"age_ms\":%u,\"stale\":%s,\"valid_flags\":%u,\"carrier\":",(unsigned long long)wide(s+4),((unsigned)s[2]<<16)|s[3],age,(age>3000||s[45]&1)?"true":"false",s[1]);
    if(s[1]&1)fprintf(out,"%u",s[18]);else fputs("null",out);
    fputs(",\"ipv4\":",out);if(s[1]&2)fprintf(out,"\"%u.%u.%u.%u\"",s[40]>>8,s[40]&255,s[41]>>8,s[41]&255);else fputs("null",out);
    fputs(",\"signal_dbm\":",out);if(s[1]&4)fprintf(out,"%d",(int16_t)s[42]);else fputs("null",out);
    fputs(",\"rx_bytes\":",out);if(s[1]&8)fprintf(out,"\"%llu\"",(unsigned long long)wide(s+32));else fputs("null",out);
    fputs(",\"tx_bytes\":",out);if(s[1]&16)fprintf(out,"\"%llu\"",(unsigned long long)wide(s+36));else fputs("null",out);
    fprintf(out,",\"mono_ms\":\"%llu\",\"wall_ms\":\"%llu\",\"rtu_enabled\":%s,\"diagnostics\":%u}",(unsigned long long)wide(s+8),(unsigned long long)wide(s+12),s[44]?"true":"false",s[45]);
    return 200;
}
static int parameters(char *query,sqlite3_int64 *before,int *limit)
{
    *before=LLONG_MAX;*limit=50;if(!*query)return 0;
    if(query[0]=='&'||query[strlen(query)-1]=='&'||strstr(query,"&&"))return -1;
    unsigned seen=0;char *save=NULL;
    for(char *p=strtok_r(query,"&",&save);p;p=strtok_r(NULL,"&",&save)) {
        char *equal=strchr(p,'=');if(!equal)return -1;
        *equal++='\0';
        if(!*equal||strspn(equal,"0123456789")!=strlen(equal))return -1;
        errno=0;char *end;long long n=strtoll(equal,&end,10);if(errno||*end||n<1)return -1;
        unsigned bit;
        if(!strcmp(p,"before")){bit=1;*before=n;}
        else if(!strcmp(p,"limit")&&n<=100){bit=2;*limit=(int)n;}
        else return -1;
        if(seen&bit)return -1;
        seen|=bit;
    }
    return 0;
}
static int scalar(sqlite3 *db,const char *sql)
{
    sqlite3_stmt *s=NULL;int value=-1;
    if(sqlite3_prepare_v2(db,sql,-1,&s,NULL)==SQLITE_OK&&sqlite3_step(s)==SQLITE_ROW)value=sqlite3_column_int(s,0);
    sqlite3_finalize(s);return value;
}
static int query_budget(void *arg) { unsigned *steps=arg;return ++*steps>100; }
static int history(FILE *out,char *query,bool events)
{
    sqlite3_int64 before;int limit;if(parameters(query,&before,&limit))return 400;
    sqlite3 *db=NULL;sqlite3_stmt *s=NULL;int result=503;
    if(sqlite3_open_v2(database,&db,SQLITE_OPEN_READONLY,NULL)!=SQLITE_OK)goto done;
    sqlite3_busy_timeout(db,200);
    sqlite3_limit(db,SQLITE_LIMIT_LENGTH,8192);
    unsigned steps=0;sqlite3_progress_handler(db,1000,query_budget,&steps);
    if(scalar(db,"PRAGMA user_version")!=1||scalar(db,"PRAGMA application_id")!=1412771889)goto done;
    const char *sql=events?"SELECT id,session,seq,wall_ms,kind FROM events WHERE id<? ORDER BY id DESC LIMIT ?":"SELECT id,session,seq,mono_ms,wall_ms,valid,carrier,ipv4,signal_dbm,registers FROM samples WHERE id<? ORDER BY id DESC LIMIT ?";
    if(sqlite3_prepare_v2(db,sql,-1,&s,NULL)!=SQLITE_OK)goto done;
    sqlite3_bind_int64(s,1,before);sqlite3_bind_int(s,2,limit+1);
    fputs("{\"source\":\"sqlite_history\",\"items\":[",out);
    int count=0,rc;sqlite3_int64 last=0;bool more=false;
    while((rc=sqlite3_step(s))==SQLITE_ROW) {
        if(count==limit){more=true;break;}
        if(count++)fputc(',',out);
        last=sqlite3_column_int64(s,0);
        fprintf(out,"{\"id\":\"%lld\",\"session\":",(long long)last);quoted(out,sqlite3_column_text(s,1));
        fprintf(out,",\"seq\":%lld",(long long)sqlite3_column_int64(s,2));
        if(events) {
            fprintf(out,",\"wall_ms\":\"%lld\",\"kind\":",(long long)sqlite3_column_int64(s,3));quoted(out,sqlite3_column_text(s,4));
        } else {
            if(sqlite3_column_bytes(s,9)!=128)goto done;
            fprintf(out,",\"mono_ms\":\"%lld\",\"wall_ms\":\"%lld\",\"valid_flags\":%d,\"carrier\":",(long long)sqlite3_column_int64(s,3),(long long)sqlite3_column_int64(s,4),sqlite3_column_int(s,5));
            if(sqlite3_column_type(s,6)==SQLITE_NULL)fputs("null",out);else fprintf(out,"%d",sqlite3_column_int(s,6));
            fputs(",\"ipv4\":",out);quoted(out,sqlite3_column_text(s,7));
            fputs(",\"signal_dbm\":",out);if(sqlite3_column_type(s,8)==SQLITE_NULL)fputs("null",out);else fprintf(out,"%d",sqlite3_column_int(s,8));
        }
        fputc('}',out);
    }
    if(rc!=SQLITE_DONE&&!more)goto done;
    fprintf(out,"],\"has_more\":%s,\"next_cursor\":",more?"true":"false");
    if(more)fprintf(out,"\"%lld\"",(long long)last);else fputs("null",out);
    fputs("}",out);result=200;
done:
    if(result==503)fprintf(stderr,"WEB_DB_UNAVAILABLE\n");
    sqlite3_finalize(s);if(db)sqlite3_close(db);return result;
}
static void handle(int fd)
{
    char method[16]={0},path[128]={0},query[512]={0};
    int status=request(fd,method,path,query);char *body=NULL;size_t bytes=0;
    FILE *out=open_memstream(&body,&bytes);if(!out)return;
    if(!status) {
        if(strcmp(method,"GET"))status=405;
        else if(!strcmp(path,"/api/v1/status"))status=*query?400:realtime(out);
        else if(!strcmp(path,"/api/v1/history"))status=history(out,query,false);
        else if(!strcmp(path,"/api/v1/events"))status=history(out,query,true);
        else status=404;
    }
    fclose(out);
    if(status!=200){free(body);body=NULL;bytes=0;out=open_memstream(&body,&bytes);if(!out)return;fprintf(out,"{\"error\":\"%s\"}",status==503?"source_unavailable":status==405?"read_only":status==404?"not_found":"invalid_request");fclose(out);}
    struct timeval timeout={.tv_sec=2,.tv_usec=0};setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    char header[256];int n=snprintf(header,sizeof(header),"Status: %d\r\nContent-Type: application/json; charset=utf-8\r\nCache-Control: no-store\r\nContent-Length: %zu\r\n\r\n",status,bytes);
    FILE *sock=fdopen(dup(fd),"w");if(sock){fwrite(header,1,(size_t)n,sock);fwrite(body,1,bytes,sock);fclose(sock);}free(body);
}
int main(int argc,char **argv)
{
    int duration=300;
    for(int i=1;i<argc;i+=2){if(i+1>=argc)return 2;
        if(!strcmp(argv[i],"--socket"))socket_path=argv[i+1];
        else if(!strcmp(argv[i],"--db"))database=argv[i+1];
        else if(!strcmp(argv[i],"--target"))target=argv[i+1];
        else if(!strcmp(argv[i],"--runtime-file")){if(argv[i+1][0]!='/')return 2;runtime_path=argv[i+1];}
        else if(!strcmp(argv[i],"--port")||!strcmp(argv[i],"--duration")) {
            char *end;errno=0;long n=strtol(argv[i+1],&end,10);if(errno||*end||n<1||n>65535)return 2;
            if(!strcmp(argv[i],"--port"))target_port=(int)n;else duration=(int)n;
        }else return 2;
    }
    struct sockaddr_un a={.sun_family=AF_UNIX};struct in_addr ip;
    if(!socket_path||socket_path[0]!='/'||strlen(socket_path)>=sizeof(a.sun_path)||!database||database[0]!='/'||inet_pton(AF_INET,target,&ip)!=1)return 2;
    strcpy(a.sun_path,socket_path);
    signal(SIGPIPE,SIG_IGN);struct sigaction act;memset(&act,0,sizeof(act));act.sa_handler=halt;sigaction(SIGINT,&act,NULL);sigaction(SIGTERM,&act,NULL);
    int listener=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(listener<0)return 1;
    if(bind(listener,(struct sockaddr*)&a,sizeof(a))){perror("SCGI bind (existing path is not removed)");close(listener);return 1;}
    if(listen(listener,16)){close(listener);unlink(socket_path);return 1;}
    printf("WEB_API_READY socket=%s read_only=true\n",socket_path);fflush(stdout);uint64_t start=ms();
    while(!stopped&&ms()-start<(uint64_t)duration*1000){struct pollfd p={.fd=listener,.events=POLLIN};if(poll(&p,1,100)<=0)continue;int fd=accept4(listener,NULL,NULL,SOCK_CLOEXEC);if(fd<0)continue;handle(fd);close(fd);}
    close(listener);unlink(socket_path);puts("WEB_API_STOPPED");return 0;
}
