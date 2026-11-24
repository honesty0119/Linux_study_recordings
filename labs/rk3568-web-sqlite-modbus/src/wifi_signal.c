/* 补充厂商驱动的信号采集：通过已有iw/nl80211路径读取。
 * posix_spawn不经过shell，250ms上限；只回收本次启动的子进程。
 */
#define _GNU_SOURCE
#include "wifi_signal.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int wifi_parse_signal(const char *text, int *dbm)
{
    for (const char *p=text; *p;) {
        const char *end=strchr(p,'\n'); size_t n=end?(size_t)(end-p):strlen(p);
        if (n<128) {
            char line[128],*q,*tail;
            memcpy(line,p,n);line[n]='\0';q=line;
            while (*q && isspace((unsigned char)*q)) ++q;
            if (strncmp(q,"signal:",7)==0) {
                q+=7;errno=0;long value=strtol(q,&tail,10);
                if (!errno && tail!=q && value>=-127 && value<=0) {
                    while (*tail && isspace((unsigned char)*tail)) ++tail;
                    if (strncmp(tail,"dBm",3)==0) {
                        tail+=3;while (*tail && isspace((unsigned char)*tail)) ++tail;
                        if (!*tail) { *dbm=(int)value;return 0; }
                    }
                }
            }
        }
        if (!end) break;
        p=end+1;
    }
    return -1;
}
static long long milliseconds(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC,&t)) return -1;
    return (long long)t.tv_sec*1000+t.tv_nsec/1000000;
}
int wifi_read_signal(const char *iface,int *dbm)
{
    int pipefd[2],status=0; pid_t pid; size_t used=0; char output[4096];
    if (pipe2(pipefd,O_CLOEXEC|O_NONBLOCK)) return -1;
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions)) { close(pipefd[0]);close(pipefd[1]);return -1; }
    int rc=posix_spawn_file_actions_adddup2(&actions,pipefd[1],STDOUT_FILENO);
    rc|=posix_spawn_file_actions_addopen(&actions,STDERR_FILENO,"/dev/null",O_WRONLY,0);
    rc|=posix_spawn_file_actions_addclose(&actions,pipefd[0]);
    char *argv[]={"/usr/sbin/iw","dev",(char*)iface,"link",NULL};
    char *env[]={"LC_ALL=C","PATH=/usr/sbin:/usr/bin:/sbin:/bin",NULL};
    if (!rc) rc=posix_spawn(&pid,argv[0],&actions,NULL,argv,env);
    posix_spawn_file_actions_destroy(&actions);close(pipefd[1]);
    if (rc) { close(pipefd[0]); return -1; }
    long long start=milliseconds(); int success=0;
    for (;;) {
        ssize_t n=read(pipefd[0],output+used,sizeof(output)-1-used);
        if (n>0) { used+=(size_t)n; if (used==sizeof(output)-1) break; }
        else if (n==0) {
            pid_t done=waitpid(pid,&status,WNOHANG);
            if (done==pid) { pid=-1; success=WIFEXITED(status)&&WEXITSTATUS(status)==0;break; }
            if (done<0 && errno!=EINTR) break;
        } else if (errno!=EAGAIN && errno!=EINTR) break;
        long long now=milliseconds();
        if (start<0 || now<0 || now-start>=250) break;
        struct pollfd p={.fd=pipefd[0],.events=POLLIN};
        (void)poll(&p,1,10);
    }
    if (pid>0) { kill(pid,SIGKILL); while (waitpid(pid,&status,0)<0 && errno==EINTR) {} }
    close(pipefd[0]);output[used]='\0';
    return success?wifi_parse_signal(output,dbm):-1;
}
