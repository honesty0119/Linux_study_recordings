/* 任务05：Modbus TCP只读主站。一次FC03读完整快照，不发送任何写功能。
 * 输出JSON Lines；有效位未置位的字段输出null，64位计数用字符串避免前端精度丢失。
 * 每次读取失败关闭连接，下轮重新连接；任何失败使最终退出码为1，即使后来恢复。
 */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <modbus.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;
static void stop_signal(int value) { (void)value; stopped=1; }
static uint32_t u32(const uint16_t *r) { return ((uint32_t)r[0]<<16)|r[1]; }
static uint64_t u64(const uint16_t *r) { return ((uint64_t)u32(r)<<32)|u32(r+2); }
static int number(const char *s, int lo, int hi)
{
    char *end; errno=0; long value=strtol(s,&end,10);
    if(errno || end==s || *end || value<lo || value>hi) {
        fprintf(stderr,"Invalid argument: %s\n",s); exit(2);
    }
    return (int)value;
}
static void usage(void)
{
    puts("tcp_status_client --host IPv4 [--port 502] [--unit 1] [--count 5] [--interval-ms 1000]");
    puts("FC03 only; map 0x0200; 64 registers at address 0. No RTU, writes or database access.");
    puts("count: 1..3600; interval-ms: 100..2000; exit 0: all reads passed, 1: any failure, 2: arguments.");
}
/* 数字IPv4 + 非阻塞connect，将建连等待限制为1秒；不依赖系统默认TCP超时。 */
static int connect_socket(const char *host, int port)
{
    int fd=socket(AF_INET,SOCK_STREAM,0);
    if(fd<0)return -1;
    int flags=fcntl(fd,F_GETFL);
    if(flags<0 || fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0)goto fail;
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons((uint16_t)port)};
    if(inet_pton(AF_INET,host,&address.sin_addr)!=1){errno=EINVAL;goto fail;}
    if(connect(fd,(struct sockaddr *)&address,sizeof(address))<0) {
        if(errno!=EINPROGRESS)goto fail;
        struct pollfd wait={.fd=fd,.events=POLLOUT};
        int rc=poll(&wait,1,1000);
        if(rc==0){errno=ETIMEDOUT;goto fail;}
        if(rc<0)goto fail;
        int error=0; socklen_t size=sizeof(error);
        if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&size)<0)goto fail;
        if(error){errno=error;goto fail;}
    }
    if(fcntl(fd,F_SETFL,flags)<0)goto fail;
    struct timeval timeout={.tv_sec=1,.tv_usec=0};
    if(setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout))<0)goto fail;
    return fd;
fail: {
    int error=errno;close(fd);errno=error;return -1;
    }
}
/* libmodbus 3.2.0通用TCP读取未核验Unit ID，故用其原始收发API并严格核验响应头。
 * 每次只有一个在途请求；任何不匹配均关闭连接，不把别的设备/事务响应当作状态。
 */
static int read_snapshot(modbus_t *ctx, int unit, uint16_t tid, uint16_t *words)
{
    const uint8_t request[6]={(uint8_t)unit,3,0,0,0,64};
    uint8_t reply[MODBUS_TCP_MAX_ADU_LENGTH];
    if(modbus_send_raw_request_tid(ctx,request,sizeof(request),tid)<0)return -1;
    int n=modbus_receive_confirmation(ctx,reply);
    if(n<0)return -1;
    if(n<9 || (((unsigned)reply[0]<<8)|reply[1])!=tid || reply[2] || reply[3] ||
       (((unsigned)reply[4]<<8)|reply[5])+6U!=(unsigned)n || reply[6]!=(uint8_t)unit) {
        errno=EMBBADDATA;return -1;
    }
    if(n==9 && reply[7]==0x83 && reply[8]>=1 && reply[8]<=11) {
        errno=MODBUS_ENOBASE+reply[8];return -1;
    }
    if(n!=137 || reply[7]!=3 || reply[8]!=128){errno=EMBBADDATA;return -1;}
    for(unsigned i=0;i<64;i++)words[i]=(uint16_t)(((unsigned)reply[9+2*i]<<8)|reply[10+2*i]);
    return 64;
}
static void counter(const char *name, const uint16_t *r, unsigned mask, int offset)
{
    printf(",\"%s\":",name);
    if(r[1]&mask)printf("\"%" PRIu64 "\"",u64(r+offset));else fputs("null",stdout);
}
static void snapshot_json(const uint16_t *r, unsigned attempt)
{
    printf("{\"poll\":%u,\"map_version\":512,\"valid_flags\":%u,\"seq\":%" PRIu32
           ",\"session\":\"%016" PRIx64 "\",\"sample_monotonic_ms\":\"%" PRIu64
           "\",\"age_ms\":%" PRIu32 ",\"stale\":%s,\"diagnostics\":%u",
           attempt,r[1],u32(r+2),u64(r+4),u64(r+8),u32(r+16),
           ((r[45]&1U)||u32(r+16)>3000)?"true":"false",r[45]);
    fputs(",\"sample_realtime_ms\":",stdout);
    if(r[1]&128U)printf("\"%" PRIu64 "\"",u64(r+12));else fputs("null",stdout);
    fputs(",\"carrier\":",stdout);
    if(r[1]&1U)printf("%u",r[18]);else fputs("null",stdout);
    fputs(",\"ipv4\":",stdout);
    if(r[1]&2U)printf("\"%u.%u.%u.%u\"",r[40]>>8,r[40]&255,r[41]>>8,r[41]&255);
    else fputs("null",stdout);
    fputs(",\"signal_dbm\":",stdout);
    if(r[1]&4U)printf("%d",r[42]>32767?(int)r[42]-65536:(int)r[42]);else fputs("null",stdout);
    counter("rx_bytes",r,8U,32);counter("tx_bytes",r,16U,36);
    counter("rx_errors",r,32U,24);counter("tx_errors",r,64U,28);
    printf(",\"counter_epoch\":%" PRIu32 ",\"interface_kind\":%u,\"serial_open\":%u,\"rtu_enabled\":%s}\n",
           u32(r+46),r[43],r[19],r[44]?"true":"false");
    fflush(stdout);
}
int main(int argc, char **argv)
{
    const char *host=NULL;int port=502,unit=1,count=5,interval=1000;
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--help")){usage();return 0;}
        if(i+1>=argc){usage();return 2;}
        const char *key=argv[i],*value=argv[++i];
        if(!strcmp(key,"--host"))host=value;
        else if(!strcmp(key,"--port"))port=number(value,1,65535);
        else if(!strcmp(key,"--unit"))unit=number(value,1,247);
        else if(!strcmp(key,"--count"))count=number(value,1,3600);
        else if(!strcmp(key,"--interval-ms"))interval=number(value,100,2000);
        else {usage();return 2;}
    }
    struct in_addr address;
    if(!host || inet_pton(AF_INET,host,&address)!=1){usage();return 2;}
    struct sigaction action={0};action.sa_handler=stop_signal;sigemptyset(&action.sa_mask);
    sigaction(SIGINT,&action,NULL);sigaction(SIGTERM,&action,NULL);signal(SIGPIPE,SIG_IGN);
    modbus_t *ctx=modbus_new_tcp(host,port);
    if(!ctx){perror("modbus_new_tcp");return 1;}
    if(modbus_set_slave(ctx,unit)<0 || modbus_set_response_timeout(ctx,1,0)<0 ||
       modbus_set_byte_timeout(ctx,0,0)<0){modbus_free(ctx);return 1;}
    int fd=-1;unsigned passed=0,failed=0;
    for(int i=0;i<count && !stopped;i++) {
        if(fd<0){
            fd=connect_socket(host,port);
            if(fd>=0 && modbus_set_socket(ctx,fd)<0){int e=errno;close(fd);fd=-1;errno=e;}
        }
        uint16_t words[64];
        if(fd<0 || read_snapshot(ctx,unit,(uint16_t)(i+1),words)!=64) {
            fprintf(stderr,"TCP_READ_FAILED poll=%d error=%s\n",i+1,modbus_strerror(errno));failed++;
            if(fd>=0){modbus_close(ctx);fd=-1;}
        } else if(words[0]!=0x0200) {
            fprintf(stderr,"TCP_MAP_UNSUPPORTED poll=%d version=0x%04x\n",i+1,words[0]);
            failed++;modbus_close(ctx);fd=-1;
        } else {snapshot_json(words,(unsigned)i+1);passed++;}
        if(i+1<count && !stopped){
            struct timespec delay={.tv_sec=interval/1000,.tv_nsec=(interval%1000)*1000000L};
            while(nanosleep(&delay,&delay)<0 && errno==EINTR && !stopped){}
        }
    }
    if(fd>=0)modbus_close(ctx);
    modbus_free(ctx);
    fprintf(stderr,"TCP_CLIENT_DONE passed=%u failed=%u interrupted=%d\n",passed,failed,(int)stopped);
    return failed||stopped?1:0;
}
