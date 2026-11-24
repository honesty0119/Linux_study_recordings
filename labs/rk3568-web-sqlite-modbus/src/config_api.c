/* 配置管理C SCGI服务：仅供私有回环nginx入口，使用SSH隧道传输。
 * Bearer令牌从0600本地文件读取，不回显/不写日志；POST要求精确Origin和revision。
 * 只保存配置，不自动重启进程、不修改网络、不打开串口。 */
#define _GNU_SOURCE
#include "config_store.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
static volatile sig_atomic_t stopped;
static const char *db_path,*runtime_path,*socket_path,*origin;
static char token[65];
static void halt(int sig){(void)sig;stopped=1;}
static uint64_t ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000U+t.tv_nsec/1000000U;}
static int exact(int fd,void *buffer,size_t size,uint64_t deadline)
{
    char *p=buffer;size_t have=0;
    while(have<size&&!stopped){uint64_t now=ms();if(now>=deadline)return -1;
        struct pollfd f={.fd=fd,.events=POLLIN};int rc=poll(&f,1,(int)(deadline-now));
        if(rc<0&&errno==EINTR)continue;
        if(rc<=0)return -1;
        ssize_t n=recv(fd,p+have,size-have,0);if(n<0&&errno==EINTR)continue;
        if(n<=0)return -1;
        have+=(size_t)n;
    }
    return have==size?0:-1;
}
struct request { char method[16],path[128],query[128],auth[80],origin[128],type[80],body[513]; };
/* SCGI长度包含首个CONTENT_LENGTH参数，nginx自动提供；正文最多512字节。 */
static int receive_request(int fd,struct request *r)
{
    char headers[4097];size_t length=0,digits=0,body_size=0;bool leading_zero=false;uint64_t deadline=ms()+1000;
    for(;;){char c;if(exact(fd,&c,1,deadline))return 400;if(c==':')break;
        if(c<'0'||c>'9'||digits==5)return 400;
        if(!digits)leading_zero=c=='0';
        ++digits;length=length*10+(unsigned)(c-'0');
    }
    if(!digits||!length||length>4096||(digits>1&&leading_zero)||exact(fd,headers,length+1,deadline)||headers[length]!=',')return 400;
    unsigned seen=0;size_t pos=0;
    while(pos<length){char *key=headers+pos;size_t n=strnlen(key,length-pos);if(!n||pos+n>=length)return 400;pos+=n+1;
        char *value=headers+pos;n=strnlen(value,length-pos);if(pos+n>=length)return 400;pos+=n+1;
        unsigned bit=0;char *dest=NULL;size_t cap=0;
        if(!strcmp(key,"CONTENT_LENGTH")){bit=1;if(seen||!n||n>3||strspn(value,"0123456789")!=n)return 400;body_size=(size_t)strtoul(value,NULL,10);if(body_size>512)return 413;}
        else if(!strcmp(key,"SCGI")){bit=2;if(strcmp(value,"1"))return 400;}
        else if(!strcmp(key,"REQUEST_METHOD")){bit=4;dest=r->method;cap=sizeof(r->method);}
        else if(!strcmp(key,"PATH_INFO")){bit=8;dest=r->path;cap=sizeof(r->path);}
        else if(!strcmp(key,"QUERY_STRING")){bit=16;dest=r->query;cap=sizeof(r->query);}
        else if(!strcmp(key,"HTTP_AUTHORIZATION")){bit=32;dest=r->auth;cap=sizeof(r->auth);}
        else if(!strcmp(key,"HTTP_ORIGIN")){bit=64;dest=r->origin;cap=sizeof(r->origin);}
        else if(!strcmp(key,"CONTENT_TYPE")){bit=128;dest=r->type;cap=sizeof(r->type);}
        else return 400;
        if(seen&bit)return 400;
        seen|=bit;
        if(dest){if(n>=cap)return 400;memcpy(dest,value,n+1);}
    }
    if((seen&31)!=31||exact(fd,r->body,body_size,deadline)||memchr(r->body,0,body_size))return 400;
    r->body[body_size]=0;
    if(!strcmp(r->method,"GET")&&body_size)return 400;
    return 0;
}
static int authorized(const char *auth)
{
    if(strlen(auth)!=71||strncmp(auth,"Bearer ",7))return 0;
    unsigned diff=0;for(unsigned i=0;i<64;i++)diff|=(unsigned char)auth[i+7]^(unsigned char)token[i];return diff==0;
}
static int load_token(const char *path)
{
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);if(fd<0)return -1;
    struct stat st;char data[66];int result=-1;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077)||st.st_size<64||st.st_size>65)goto done;
    ssize_t n=read(fd,data,sizeof(data));if(n!=64&&(n!=65||data[64]!='\n'))goto done;
    for(int i=0;i<64;i++)if(!strchr("0123456789abcdef",data[i])||!data[i])goto done;
    memcpy(token,data,64);result=0;
done:memset(data,0,sizeof(data));close(fd);return result;
}
static int number(const char *s,int *value)
{
    if(!*s||strspn(s,"0123456789")!=strlen(s))return -1;
    char *end;errno=0;long n=strtol(s,&end,10);if(errno||*end||n<1||n>=INT_MAX)return -1;*value=(int)n;return 0;
}
static int parse_config(char *body,struct task_config *c,int *expected)
{
    if(!*body||body[0]=='&'||body[strlen(body)-1]=='&'||strstr(body,"&&"))return -1;
    unsigned seen=0;char *save=NULL;
    for(char *part=strtok_r(body,"&",&save);part;part=strtok_r(NULL,"&",&save)){
        char *v=strchr(part,'=');if(!v)return -1;*v++=0;unsigned bit;int *target;
        if(!strcmp(part,"revision")){bit=1;target=expected;}
        else if(!strcmp(part,"unit")){bit=2;target=&c->unit;}
        else if(!strcmp(part,"baud")){bit=4;target=&c->baud;}
        else if(!strcmp(part,"data_bits")){bit=8;target=&c->data_bits;}
        else if(!strcmp(part,"stop_bits")){bit=16;target=&c->stop_bits;}
        else if(!strcmp(part,"history_seconds")){bit=32;target=&c->history_seconds;}
        else if(!strcmp(part,"parity")){bit=64;target=NULL;if(strlen(v)!=1||!strchr("NEO",v[0]))return -1;c->parity=v[0];}
        else return -1;
        if(seen&bit)return -1;
        seen|=bit;if(target&&number(v,target))return -1;
    }
    c->revision=*expected;return seen==127&&config_valid(c)?0:-1;
}
static void response_config(FILE *out,const struct task_config *c)
{
    struct task_runtime r;bool live=runtime_read(runtime_path,&r)==0;
    fprintf(out,"{\"saved\":{\"revision\":%d,\"unit\":%d,\"baud\":%d,\"data_bits\":%d,\"stop_bits\":%d,\"parity\":\"%c\",\"history_seconds\":%d},\"service_state\":\"%s\",\"serial_state\":\"disabled_hardware_pending\",\"runtime\":",c->revision,c->unit,c->baud,c->data_bits,c->stop_bits,c->parity,c->history_seconds,!live?"runtime_unavailable":r.revision==c->revision?"loaded_at_start":"pending_restart");
    if(live)fprintf(out,"{\"revision\":%d,\"unit\":%d,\"history_seconds\":%d,\"history_enabled\":%s,\"session\":\"%016llx\"}",r.revision,r.unit,r.history_seconds,r.history_enabled?"true":"false",(unsigned long long)r.session);
    else fputs("null",out);
    fputs("}",out);
}
static void handle(int fd)
{
    struct request r={0};int status=receive_request(fd,&r);char *body=NULL;size_t size=0;
    FILE *out=open_memstream(&body,&size);if(!out)return;
    struct task_config c={0};
    if(!status){
        if(strcmp(r.path,"/admin/api/v1/config"))status=404;
        else if(strcmp(r.method,"GET")&&strcmp(r.method,"POST"))status=405;
        else if(!authorized(r.auth))status=401;
        else if(*r.query)status=400;
        else if(!strcmp(r.method,"GET"))status=config_load(db_path,&c)?503:200;
        else if(strcmp(r.origin,origin))status=403;
        else if(strcmp(r.type,"application/x-www-form-urlencoded"))status=415;
        else {int expected=0;status=parse_config(r.body,&c,&expected)?400:config_save(db_path,expected,&c);}
    }
    if(status==200)response_config(out,&c);
    else fprintf(out,"{\"error\":\"%s\"}",status==401?"authentication_required":status==403?"origin_rejected":status==409?"revision_conflict":status==503?"configuration_unavailable":status==404?"not_found":status==405?"method_not_allowed":"invalid_request");
    fclose(out);struct timeval timeout={.tv_sec=2};setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    FILE *sock=fdopen(dup(fd),"w");if(sock){fprintf(sock,"Status: %d\r\nContent-Type: application/json; charset=utf-8\r\nCache-Control: no-store\r\nContent-Length: %zu\r\n\r\n",status,size);fwrite(body,1,size,sock);fclose(sock);}free(body);
}
int main(int argc,char **argv)
{
    const char *token_path=NULL;int duration=300;
    if(argc==3&&!strcmp(argv[1],"--init-db")){if(argv[2][0]!='/')return 2;umask(077);return config_init(argv[2])?1:0;}
    for(int i=1;i<argc;i+=2){if(i+1>=argc)return 2;
        if(!strcmp(argv[i],"--socket"))socket_path=argv[i+1];
        else if(!strcmp(argv[i],"--config-db"))db_path=argv[i+1];
        else if(!strcmp(argv[i],"--runtime-file"))runtime_path=argv[i+1];
        else if(!strcmp(argv[i],"--token-file"))token_path=argv[i+1];
        else if(!strcmp(argv[i],"--origin"))origin=argv[i+1];
        else if(!strcmp(argv[i],"--duration")){if(number(argv[i+1],&duration)||duration>86400)return 2;}
        else return 2;
    }
    struct sockaddr_un a={.sun_family=AF_UNIX};int origin_port;
    if(!socket_path||socket_path[0]!='/'||strlen(socket_path)>=sizeof(a.sun_path)||!db_path||db_path[0]!='/'||!runtime_path||runtime_path[0]!='/'||!token_path||token_path[0]!='/'||!origin||strncmp(origin,"http://127.0.0.1:",17)||number(origin+17,&origin_port)||origin_port>65535)return 2;
    umask(077);
    if(load_token(token_path)){fprintf(stderr,"CONFIG_AUTH_FILE_INVALID\n");return 1;}
    if(config_init(db_path)){fprintf(stderr,"CONFIG_DB_UNAVAILABLE\n");return 1;}
    signal(SIGPIPE,SIG_IGN);struct sigaction act={0};act.sa_handler=halt;sigaction(SIGTERM,&act,NULL);sigaction(SIGINT,&act,NULL);
    int listener=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(listener<0)return 1;
    strcpy(a.sun_path,socket_path);
    if(bind(listener,(struct sockaddr*)&a,sizeof(a))){perror("CONFIG bind (existing path retained)");close(listener);return 1;}
    if(listen(listener,8)){close(listener);unlink(socket_path);return 1;}
    printf("CONFIG_API_READY auth=token apply=next_task_start\n");fflush(stdout);uint64_t start=ms();
    while(!stopped&&ms()-start<(uint64_t)duration*1000U){struct pollfd f={.fd=listener,.events=POLLIN};if(poll(&f,1,100)<=0)continue;int fd=accept4(listener,NULL,NULL,SOCK_CLOEXEC);if(fd<0)continue;handle(fd);close(fd);}
    close(listener);unlink(socket_path);memset(token,0,sizeof(token));puts("CONFIG_API_STOPPED");return 0;
}
