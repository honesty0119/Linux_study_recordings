/* 任务05第一阶段：Linux接口状态采集与Modbus TCP只读服务器。
 * 主线程每秒发布快照；最多4个客户端各持有独立libmodbus上下文和映射。
 * 显式--db启用异步SQLite历史；不修改网络、不打开UART。
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <linux/wireless.h>
#include <modbus.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "history.h"
#include "config_store.h"
#include "wifi_signal.h"

#define REG_COUNT 64
#define CLIENT_MAX 4
static uint16_t snapshot[REG_COUNT];
static pthread_mutex_t snap_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t clients_done = PTHREAD_COND_INITIALIZER;
static int client_fds[CLIENT_MAX] = {-1,-1,-1,-1};
/* accept4时保存对端端口；shutdown/RST后不再向内核查询已断开的连接。 */
static unsigned client_peer_ports[CLIENT_MAX];
static unsigned clients_count;
static atomic_bool quitting;
static volatile sig_atomic_t interrupted;
static char interface_name[IFNAMSIZ] = "wlan0";
static char bind_address[INET_ADDRSTRLEN] = "127.0.0.1";
static int port = 1502, unit_id = 1;
static uint64_t session_id;
static const char *runtime_path;
static bool runtime_owned;
static struct task_runtime active_config;
/* 诊断显式启用；最多输出1024条连接事件，防止轮询造成无限日志增长。 */
static bool trace_connections;
static atomic_uint_fast64_t next_connection_id, trace_events, trace_suppressed;
static uint64_t now_ms(clockid_t clock);
static void connection_log(uint64_t id, unsigned peer_port, const char *reason, int error,
                           unsigned requests, int tid, uint64_t started)
{
    if (!trace_connections) return;
    if (atomic_fetch_add(&trace_events,1)>=1024) { atomic_fetch_add(&trace_suppressed,1); return; }
    uint64_t mono=now_ms(CLOCK_MONOTONIC);
    fprintf(stderr,"TCP_CLOSE id=%llu peer_port=%u reason=%s errno=%d requests=%u last_tid=%d elapsed_ms=%llu mono_ms=%llu\n",
            (unsigned long long)id,peer_port,reason,error,requests,tid,
            (unsigned long long)(mono-started),(unsigned long long)mono);
}

/* 单调时钟用于期限；运行中时钟读取失败则终止，避免错误的超时判断。 */
static uint64_t now_ms(clockid_t clock)
{
    struct timespec t;
    if (clock_gettime(clock, &t) != 0) { perror("clock_gettime"); exit(1); }
    return (uint64_t)t.tv_sec * 1000U + (uint64_t)t.tv_nsec / 1000000U;
}
static void put32(uint16_t *p, uint32_t x) { p[0]=(uint16_t)(x>>16); p[1]=(uint16_t)x; }
static void put64(uint16_t *p, uint64_t x) { put32(p,(uint32_t)(x>>32)); put32(p+2,(uint32_t)x); }
static uint64_t get64(const uint16_t *p)
{
    return ((uint64_t)p[0]<<48)|((uint64_t)p[1]<<32)|((uint64_t)p[2]<<16)|p[3];
}
static void on_signal(int sig) { (void)sig; interrupted=1; }

/* sysfs读取失败意味着未知，不把失败当作有效的零值。 */
static int read_counter(const char *leaf, uint64_t *value)
{
    char path[256], text[80], *end;
    unsigned long long n;
    snprintf(path,sizeof(path),"/sys/class/net/%s/%s",interface_name,leaf);
    FILE *f=fopen(path,"r");
    if (!f) return -1;
    if (!fgets(text,sizeof(text),f)) { fclose(f); return -1; }
    fclose(f);
    if (text[0]<'0' || text[0]>'9') return -1;
    errno=0; n=strtoull(text,&end,10);
    if (errno || end==text) return -1;
    while (*end==' ' || *end=='\n' || *end=='\t') ++end;
    if (*end) return -1;
    *value=(uint64_t)n;
    return 0;
}

/* 发布v2快照。单次采集是顺序读取，协议响应保证内存快照一致而非硬件原子采样。 */
static void sample_state(void)
{
    static uint32_t seq, epoch;
    static uint64_t previous_index;
    static uint64_t previous[4];
    static bool had[4];
    uint16_t s[REG_COUNT]={0};
    const char *names[]={"statistics/rx_bytes","statistics/tx_bytes",
                         "statistics/rx_errors","statistics/tx_errors"};
    const int offsets[]={32,36,24,28};
    uint64_t value;
    bool reset=false;
    char path[256]; struct stat st;
    s[0]=0x0200;
    uint64_t index=0;
    if (read_counter("ifindex",&index)==0) {
        if (previous_index && previous_index!=index) { reset=true; memset(had,0,sizeof(had)); }
        previous_index=index;
    }
    put32(s+2,++seq); put64(s+4,session_id);
    if (read_counter("carrier",&value)==0 && value<=1) { s[1]|=1U; s[18]=(uint16_t)value; }
    struct ifaddrs *addresses=NULL;
    uint32_t ipv4=UINT32_MAX; bool found=false;
    if (getifaddrs(&addresses)==0) {
        for (struct ifaddrs *a=addresses;a;a=a->ifa_next) {
            if (a->ifa_addr && a->ifa_addr->sa_family==AF_INET &&
                strcmp(a->ifa_name,interface_name)==0) {
                uint32_t ip=ntohl(((struct sockaddr_in*)a->ifa_addr)->sin_addr.s_addr);
                if (!found || ip<ipv4) ipv4=ip;
                found=true;
            }
        }
        freeifaddrs(addresses);
    }
    if (found) { s[1]|=2U; put32(s+40,ipv4); }
    snprintf(path,sizeof(path),"/sys/class/net/%s/wireless",interface_name);
    bool wifi=stat(path,&st)==0 && S_ISDIR(st.st_mode);
    s[43]=wifi?1:0;
    if (wifi) {
        int fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);
        if (fd>=0) {
            struct iwreq req; struct iw_statistics stats;
            memset(&req,0,sizeof(req)); memset(&stats,0,sizeof(stats));
            snprintf(req.ifr_name,sizeof(req.ifr_name),"%s",interface_name);
            req.u.data.pointer=&stats; req.u.data.length=sizeof(stats); req.u.data.flags=1;
            if (ioctl(fd,SIOCGIWSTATS,&req)==0 &&
                (stats.qual.updated & IW_QUAL_DBM) && !(stats.qual.updated & IW_QUAL_LEVEL_INVALID)) {
                int level=stats.qual.level;
                if (level>=64) level-=256;
                s[42]=(uint16_t)(int16_t)level; s[1]|=4U;
            }
            close(fd);
        }
        if (!(s[1]&4U)) {
            int dbm;
            if (wifi_read_signal(interface_name,&dbm)==0) { s[42]=(uint16_t)(int16_t)dbm;s[1]|=4U; }
        }
    }
    for (unsigned i=0;i<4;i++) {
        if (read_counter(names[i],&value)==0) {
            s[1]|=(uint16_t)(1U<<(3+i)); put64(s+offsets[i],value);
            if (had[i] && value<previous[i]) reset=true;
            previous[i]=value; had[i]=true;
        } else { had[i]=false; }
    }
    if (reset) ++epoch;
    put32(s+46,epoch);
    put64(s+8,now_ms(CLOCK_MONOTONIC));
    put64(s+12,now_ms(CLOCK_REALTIME)); s[1]|=128U;
    s[45]|=(uint16_t)history_diagnostics();
    pthread_mutex_lock(&snap_lock); memcpy(snapshot,s,sizeof(s)); pthread_mutex_unlock(&snap_lock);
    history_offer(s);
    if(runtime_owned){
        active_config.mono_ms=now_ms(CLOCK_MONOTONIC);active_config.session=session_id;
        if(runtime_write(runtime_path,&active_config))fprintf(stderr,"CONFIG_RUNTIME_WRITE_FAILED\n");
    }
}
static void copy_snapshot(uint16_t *s)
{
    pthread_mutex_lock(&snap_lock); memcpy(s,snapshot,sizeof(snapshot)); pthread_mutex_unlock(&snap_lock);
    uint64_t age=now_ms(CLOCK_MONOTONIC)-get64(s+8);
    put32(s+16,age>UINT32_MAX?UINT32_MAX:(uint32_t)age);
    if (age>3000) s[45]|=1U;
    s[45]|=(uint16_t)history_diagnostics();
}

/* 等待下一帧与接收完整ADU共用3秒期限；保持既有策略，不用日志改动隐式放宽超时。
 * 返回值区分对端关闭、空闲超时、半帧超时、系统错误和服务停止。 */
enum receive_result { RX_OK, RX_PEER_CLOSED, RX_IDLE_TIMEOUT, RX_PARTIAL_TIMEOUT,
                      RX_SYSTEM_ERROR, RX_SHUTDOWN };
static int receive_exact(int fd, uint8_t *dst, size_t size, uint64_t deadline, bool header)
{
    size_t have=0;
    while (have<size && !atomic_load(&quitting)) {
        uint64_t now=now_ms(CLOCK_MONOTONIC);
        if (now>=deadline) return header && !have?RX_IDLE_TIMEOUT:RX_PARTIAL_TIMEOUT;
        struct pollfd p={.fd=fd,.events=POLLIN};
        int rc=poll(&p,1,(int)(deadline-now));
        if (rc<0 && errno==EINTR) continue;
        if (rc<0) return RX_SYSTEM_ERROR;
        if (rc==0) return header && !have?RX_IDLE_TIMEOUT:RX_PARTIAL_TIMEOUT;
        ssize_t n=recv(fd,dst+have,size-have,0);
        if (n<0 && errno==EINTR) continue;
        if (n<0) return RX_SYSTEM_ERROR;
        if (n==0) return RX_PEER_CLOSED;
        have+=(size_t)n;
    }
    return atomic_load(&quitting)?RX_SHUTDOWN:RX_OK;
}
static void *client_worker(void *arg)
{
    int slot=(int)(intptr_t)arg;
    pthread_mutex_lock(&clients_lock);
    int fd=client_fds[slot]; unsigned peer_port=client_peer_ports[slot];
    pthread_mutex_unlock(&clients_lock);
    uint64_t id=atomic_fetch_add(&next_connection_id,1)+1, opened=now_ms(CLOCK_MONOTONIC);
    const char *reason="setup_error"; int saved_errno=0, last_tid=-1; unsigned requests=0;
    modbus_t *ctx=modbus_new_tcp(bind_address,port);
    modbus_mapping_t *map=modbus_mapping_new(0,0,REG_COUNT,0);
    if (!ctx || !map) { saved_errno=errno; goto done; }
    if (modbus_set_socket(ctx,fd)<0 || modbus_set_slave(ctx,unit_id)<0) { saved_errno=errno; goto done; }
    struct timeval send_timeout={.tv_sec=2,.tv_usec=0};
    if (setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&send_timeout,sizeof(send_timeout))<0) { saved_errno=errno; goto done; }
    while (!atomic_load(&quitting)) {
        uint8_t request[MODBUS_TCP_MAX_ADU_LENGTH];
        uint64_t deadline=now_ms(CLOCK_MONOTONIC)+3000;
        int received=receive_exact(fd,request,7,deadline,true);
        const char *phase="header";
        if (received==RX_OK) {
            last_tid=((int)request[0]<<8)|request[1];
            unsigned length=((unsigned)request[4]<<8)|request[5];
            if (request[2] || request[3] || length<2 || length>254) { reason="invalid_mbap"; break; }
            phase="payload";
            received=receive_exact(fd,request+7,length-1,deadline,false);
        }
        if (received!=RX_OK) {
            if (received==RX_SYSTEM_ERROR) { reason="receive_error"; saved_errno=errno; }
            else if (received==RX_PEER_CLOSED) reason="peer_closed";
            else if (received==RX_IDLE_TIMEOUT) reason="idle_timeout";
            else if (received==RX_SHUTDOWN) reason="server_shutdown";
            else reason=!strcmp(phase,"header")?"header_timeout":"payload_timeout";
            break;
        }
        unsigned length=((unsigned)request[4]<<8)|request[5];
        ++requests;
        if (request[6]!=(uint8_t)unit_id) continue;
        unsigned count=length+6, exception=0;
        /* 唯一白名单FC03；必须在通用回复前拒绝所有其他功能，包括0x16和0x17。 */
        if (request[7]!=3) exception=MODBUS_EXCEPTION_ILLEGAL_FUNCTION;
        else if (count!=12) exception=MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE;
        else {
            unsigned start=((unsigned)request[8]<<8)|request[9];
            unsigned quantity=((unsigned)request[10]<<8)|request[11];
            if (quantity<1 || quantity>125) exception=MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE;
            else if (start>=REG_COUNT || quantity>REG_COUNT-start) exception=MODBUS_EXCEPTION_ILLEGAL_DATA_ADDRESS;
        }
        int rc;
        if (exception) rc=modbus_reply_exception(ctx,request,exception);
        else { copy_snapshot(map->tab_registers); rc=modbus_reply(ctx,request,(int)count,map); }
        if (rc<0) { reason="send_error"; saved_errno=errno; break; }
    }
done:
    if (atomic_load(&quitting)) { reason="server_shutdown"; saved_errno=0; }
    connection_log(id,peer_port,reason,saved_errno,requests,last_tid,opened);
    if (map) modbus_mapping_free(map);
    if (ctx) modbus_free(ctx);
    pthread_mutex_lock(&clients_lock);
    close(fd); client_fds[slot]=-1; client_peer_ports[slot]=0; --clients_count;
    pthread_cond_broadcast(&clients_done); pthread_mutex_unlock(&clients_lock);
    return NULL;
}
static int number(const char *s, int low, int high)
{
    char *end; errno=0; long n=strtol(s,&end,10);
    if (errno || end==s || *end || n<low || n>high) { fprintf(stderr,"Invalid numeric argument: %s\n",s); exit(2); }
    return (int)n;
}
int main(int argc, char **argv)
{
    bool once=false,config_override=false; int duration=300; const char *db_path=NULL,*config_path=NULL; unsigned history_interval=10;
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--once")) { once=true; continue; }
        if (i+1>=argc) { fprintf(stderr,"Missing argument\n"); return 2; }
        const char *key=argv[i],*val=argv[++i];
        if (!strcmp(key,"--interface")) {
            if (!*val || strlen(val)>=sizeof(interface_name) || strspn(val,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.:-")!=strlen(val)) return 2;
            strcpy(interface_name,val);
        } else if (!strcmp(key,"--bind")) {
            struct in_addr a;
            if (inet_pton(AF_INET,val,&a)!=1) return 2;
            snprintf(bind_address,sizeof(bind_address),"%s",val);
        } else if (!strcmp(key,"--port")) port=number(val,1,65535);
        else if (!strcmp(key,"--trace-connections")) trace_connections=number(val,0,1)!=0;
        else if (!strcmp(key,"--unit")) { unit_id=number(val,1,247);config_override=true; }
        else if (!strcmp(key,"--config-db")) { if(val[0]!='/')return 2;config_path=val; }
        else if (!strcmp(key,"--runtime-file")) { if(val[0]!='/')return 2;runtime_path=val; }
        else if (!strcmp(key,"--db")) { if (val[0]!='/') return 2; db_path=val; }
        else if (!strcmp(key,"--history-interval")) { history_interval=(unsigned)number(val,1,3600);config_override=true; }
        else if (!strcmp(key,"--duration")) duration=number(val,1,86400);
        else { fprintf(stderr,"Unknown option: %s\n",key); return 2; }
    }
    /* 配置显式启用；与命令行覆盖冲突时拒绝，避免保存值与实际值产生歧义。 */
    if((config_path!=NULL)!=(runtime_path!=NULL))return 2;
    if(config_path){
        struct task_config c;
        if(once||config_override||(db_path&&!strcmp(db_path,config_path)))return 2;
        if(config_load(config_path,&c)){fprintf(stderr,"CONFIG_LOAD_FAILED\n");return 1;}
        unit_id=c.unit;history_interval=(unsigned)c.history_seconds;
        active_config=(struct task_runtime){.revision=c.revision,.unit=c.unit,.history_seconds=c.history_seconds};
    }
    char path[256]; struct stat st;
    snprintf(path,sizeof(path),"/sys/class/net/%s",interface_name);
    if (stat(path,&st)!=0) { perror("interface"); return 1; }
    uint64_t initial_index;
    if (read_counter("ifindex",&initial_index)!=0 || initial_index==0) { fprintf(stderr,"Not a network interface\n"); return 1; }
    int random_fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);
    if (random_fd<0) return 1;
    ssize_t n=read(random_fd,&session_id,sizeof(session_id)); close(random_fd);
    if (n!=(ssize_t)sizeof(session_id)) return 1;
    if (once && db_path) { fprintf(stderr,"--once cannot write history\n");return 2; }
    sample_state();
    if (once) {
        uint16_t s[REG_COUNT]; copy_snapshot(s);
        printf("{\"interface\":\"%s\",\"map_version\":512,\"valid_flags\":%u,\"carrier\":%u,\"ipv4\":\"%u.%u.%u.%u\",\"signal_dbm\":%d,\"signal_valid\":%s,\"rtu_enabled\":false}\n",
            interface_name,s[1],s[18],s[40]>>8,s[40]&255,s[41]>>8,s[41]&255,
            (int)(int16_t)s[42],(s[1]&4)?"true":"false");
        return 0;
    }
    struct sigaction action; memset(&action,0,sizeof(action)); action.sa_handler=on_signal;
    sigemptyset(&action.sa_mask); sigaction(SIGINT,&action,NULL); sigaction(SIGTERM,&action,NULL);
    signal(SIGPIPE,SIG_IGN);
    modbus_t *listener_ctx=modbus_new_tcp(bind_address,port);
    if (!listener_ctx) { perror("modbus_new_tcp"); return 1; }
    int listener=modbus_tcp_listen(listener_ctx,CLIENT_MAX);
    if (listener<0) { perror("listen"); modbus_free(listener_ctx); return 1; }
    int flags=fcntl(listener,F_GETFL);
    if (flags<0 || fcntl(listener,F_SETFL,flags|O_NONBLOCK)<0) { perror("fcntl"); close(listener); modbus_free(listener_ctx); return 1; }
    int history_started=history_start(db_path,history_interval);
    if(runtime_path){
        active_config.history_enabled=db_path&&history_started==0;
        if(runtime_claim(runtime_path)){fprintf(stderr,"CONFIG_RUNTIME_PATH_UNAVAILABLE\n");history_finish();close(listener);modbus_free(listener_ctx);return 1;}
        runtime_owned=true;active_config.mono_ms=now_ms(CLOCK_MONOTONIC);active_config.session=session_id;
        if(runtime_write(runtime_path,&active_config)){unlink(runtime_path);history_finish();close(listener);modbus_free(listener_ctx);return 1;}
    }
    sample_state();
    printf("READY interface=%s bind=%s port=%d unit=%d map=0x0200 duration=%d rtu=disabled\n",interface_name,bind_address,port,unit_id,duration); fflush(stdout);
    uint64_t started=now_ms(CLOCK_MONOTONIC),next=started+1000;
    while (!interrupted && now_ms(CLOCK_MONOTONIC)-started<(uint64_t)duration*1000U) {
        if (now_ms(CLOCK_MONOTONIC)>=next) { sample_state(); next=now_ms(CLOCK_MONOTONIC)+1000; }
        struct pollfd p={.fd=listener,.events=POLLIN};
        if (poll(&p,1,100)<=0) continue;
        struct sockaddr_in peer={0}; socklen_t peer_size=sizeof(peer);
        int fd=accept4(listener,(struct sockaddr*)&peer,&peer_size,SOCK_CLOEXEC);
        if (fd<0) continue;
        unsigned peer_port=ntohs(peer.sin_port);
        pthread_mutex_lock(&clients_lock);
        int slot;
        for (slot=0;slot<CLIENT_MAX && client_fds[slot]>=0;slot++) {}
        if (slot==CLIENT_MAX) {
            connection_log(atomic_fetch_add(&next_connection_id,1)+1,peer_port,"capacity_rejected",0,0,-1,now_ms(CLOCK_MONOTONIC));
            close(fd);
        }
        else {
            pthread_t thread; pthread_attr_t attr;
            pthread_attr_init(&attr); pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
            client_fds[slot]=fd; client_peer_ports[slot]=peer_port; ++clients_count;
            int thread_error=pthread_create(&thread,&attr,client_worker,(void*)(intptr_t)slot);
            if (thread_error!=0) {
                connection_log(atomic_fetch_add(&next_connection_id,1)+1,peer_port,"thread_create_error",thread_error,0,-1,now_ms(CLOCK_MONOTONIC));
                close(fd); client_fds[slot]=-1; client_peer_ports[slot]=0; --clients_count;
            }
            pthread_attr_destroy(&attr);
        }
        pthread_mutex_unlock(&clients_lock);
    }
    fprintf(stderr,"SERVER_STOP reason=%s elapsed_ms=%llu\n",interrupted?"signal":"duration",
            (unsigned long long)(now_ms(CLOCK_MONOTONIC)-started));
    close(listener); modbus_free(listener_ctx); atomic_store(&quitting,true);
    pthread_mutex_lock(&clients_lock);
    for (int i=0;i<CLIENT_MAX;i++) if (client_fds[i]>=0) shutdown(client_fds[i],SHUT_RDWR);
    while (clients_count) pthread_cond_wait(&clients_done,&clients_lock);
    pthread_mutex_unlock(&clients_lock);
    if(runtime_owned)unlink(runtime_path);
    int history_result=history_finish();
    if (trace_connections) fprintf(stderr,"TCP_TRACE_END suppressed=%llu\n",(unsigned long long)atomic_load(&trace_suppressed));
    puts("STOPPED"); return history_result?1:0;
}
