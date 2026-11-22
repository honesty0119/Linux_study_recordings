/*
 * 原任务 TCP 阶段：主线程采样/10ms 消抖 -> System V 队列 -> 网络线程。
 * 网络线程用 eventfd 唤醒和非阻塞 Socket，把心跳、重连与按键发送放在同一循环。
 * 不改变设备网络配置。SIGINT/SIGTERM 后回收本程序创建的队列和文件描述符。
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "task2_core.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/input.h>
#include <net/if.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/msg.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* 仅队列含序号/本机单调时间；线上帧严格保持原文 8 字节。 */
struct key_message {
    long type;
    uint32_t sequence;
    uint8_t frame[8];
    int64_t queued_ms;
};
#define MESSAGE_SIZE (sizeof(struct key_message) - sizeof(long))
struct context {
    int qid, wake_fd;
    struct sockaddr_in peer;
    atomic_bool stop, failed;
    unsigned int sent, uncertain; /* 网络线程写，join 后主线程读。 */
};
static volatile sig_atomic_t interrupted;

/* 所有周期/超时用单调时钟；系统时间只放入协议字段用于显示。 */
static int64_t mono_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) {
        perror("clock_gettime");
        abort();
    }
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void on_signal(int number) { (void)number; interrupted = 1; }

/* eventfd 只作唤醒；事件内容仍通过原文要求的 System V 消息队列传递。 */
static void wake_network(struct context *ctx)
{
    uint64_t one = 1;
    ssize_t n;
    do { n = write(ctx->wake_fd, &one, sizeof(one)); }
    while (n < 0 && errno == EINTR);
    if (n < 0 && errno != EAGAIN) {
        perror("eventfd write");
        atomic_store(&ctx->failed, true);
    }
}

/* 队列满时明确停止并报错，避免阻塞采样或悄悄丢失事件。 */
static int enqueue(struct context *ctx, unsigned int *count)
{
    struct key_message msg = {0};
    msg.type = 2;
    msg.sequence = *count + 1;
    msg.queued_ms = mono_ms();
    encode_frame(msg.frame, true, (uint32_t)time(NULL));
    int rc;
    do { rc = msgsnd(ctx->qid, &msg, MESSAGE_SIZE, IPC_NOWAIT); }
    while (rc < 0 && errno == EINTR);
    if (rc < 0) { perror("msgsnd: queue full/unavailable"); return -1; }
    ++*count;
    printf("[enqueue] seq=%u code=115 mono_ms=%lld\n",
           *count, (long long)msg.queued_ms);
    wake_network(ctx);
    return 0;
}

#ifndef TASK2_UDP
#define TRANSPORT_NAME "TCP"
/* 创建单次连接。连接耗时不阻塞采样线程，也不延迟下一次网络循环。 */
static int start_connect(struct context *ctx, bool *connecting)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0) {
        close(fd);
        return -1;
    }
#ifdef TCP_USER_TIMEOUT
    /* 未获 ACK 的数据最多等待约 3s；不把单向心跳误称为即时断网检测。 */
    unsigned int timeout = 3000;
    if (setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &timeout, sizeof(timeout)) < 0)
        perror("TCP_USER_TIMEOUT (using kernel default)");
#endif
    int rc = connect(fd, (struct sockaddr *)&ctx->peer, sizeof(ctx->peer));
    if (rc == 0) { *connecting = false; return fd; }
    if (errno == EINPROGRESS) { *connecting = true; return fd; }
    int saved = errno;
    close(fd);
    errno = saved;
    return -1;
}

/*
 * 已连接 Socket 可能在本机 Wi-Fi 关闭后仍接受 send，单靠 TCP 超时发现较慢。
 * 检查该 Socket 的本地 IPv4 是否仍属于 UP 接口；只读接口状态，不修改网络。
 * 返回 1=存在，0=地址已消失，-1=查询失败。不能据此检测远端静默丢包。
 */
static int local_address_present(int fd)
{
    struct sockaddr_in local;
    socklen_t size = sizeof(local);
    if (getsockname(fd, (struct sockaddr *)&local, &size) < 0) return -1;
    struct ifaddrs *addresses = NULL;
    if (getifaddrs(&addresses) < 0) return -1;
    int present = 0;
    for (struct ifaddrs *it = addresses; it; it = it->ifa_next) {
        if (it->ifa_addr && it->ifa_addr->sa_family == AF_INET && (it->ifa_flags & IFF_UP)) {
            struct sockaddr_in *a = (struct sockaddr_in *)it->ifa_addr;
            if (a->sin_addr.s_addr == local.sin_addr.s_addr) { present = 1; break; }
        }
    }
    freeifaddrs(addresses);
    return present;
}

/* 一个线程独占 Socket，部分发送不会与其他帧交错。无帧时优先检查心跳。 */
static void *network_thread(void *argument)
{
    struct context *ctx = argument;
    int fd = -1;
    bool connecting = false, key = false;
    uint8_t frame[8];
    size_t length = 0, offset = 0;
    struct key_message message = {0};
    int64_t next_connect = 0, connect_deadline = 0;
    int64_t heartbeat_due = mono_ms(), send_deadline = 0;
    int64_t next_link_check = 0; /* 本机地址状态每 100ms 查询一次。 */
    while (!atomic_load(&ctx->stop)) {
        int64_t now = mono_ms();
        if (fd < 0 && now >= next_connect) {
            next_connect = now + 1000;
            connect_deadline = now + 1000;
            printf("[tcp] connect attempt mono_ms=%lld\n", (long long)now);
            fd = start_connect(ctx, &connecting);
            if (fd < 0) perror("connect");
            else if (!connecting) {
                printf("[tcp] connected\n");
                heartbeat_due = now;
            }
        }
        /* 消费一条再返回循环，持续按键也不能饿死心跳。 */
        if (fd >= 0 && !connecting && length == 0) {
            if (now >= heartbeat_due) {
                length = encode_frame(frame, false, (uint32_t)time(NULL));
                key = false;
                do { heartbeat_due += 1000; } while (heartbeat_due <= now);
            } else {
                ssize_t n = msgrcv(ctx->qid, &message, MESSAGE_SIZE, 0, IPC_NOWAIT);
                if (n >= 0) {
                    if ((size_t)n != MESSAGE_SIZE || message.type != 2) {
                        fprintf(stderr, "Invalid queue payload\n");
                        atomic_store(&ctx->failed, true);
                        break;
                    }
                    memcpy(frame, message.frame, 8);
                    length = 8;
                    key = true;
                } else if (errno != ENOMSG && errno != EINTR) {
                    perror("msgrcv");
                    atomic_store(&ctx->failed, true);
                    break;
                }
            }
            if (length) { offset = 0; send_deadline = now + 500; }
        }
        struct pollfd p[2] = {
            { .fd = ctx->wake_fd, .events = POLLIN },
            { .fd = fd, .events = POLLIN }
        };
        if (connecting || length) p[1].events |= POLLOUT;
        /* 20ms 有界等待确保无按键时仍调度；eventfd 使新按键立即唤醒。 */
        int wait_ms = 20;
        if (fd >= 0 && !connecting && heartbeat_due > now &&
            heartbeat_due - now < wait_ms) wait_ms = (int)(heartbeat_due - now);
        int rc = poll(p, 2, wait_ms);
        if (rc < 0) {
            if (errno == EINTR) continue;
            perror("network poll");
            atomic_store(&ctx->failed, true);
            break;
        }
        if (p[0].revents & POLLIN) {
            uint64_t value;
            while (read(ctx->wake_fd, &value, sizeof(value)) < 0 && errno == EINTR) {}
        }
        now = mono_ms();
        bool disconnected = false;
        if (fd >= 0 && connecting) {
            if (p[1].revents & (POLLOUT | POLLERR | POLLHUP)) {
                int error = 0;
                socklen_t size = sizeof(error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error) {
                    fprintf(stderr, "[tcp] connect failed: %s\n",
                            strerror(error ? error : errno));
                    disconnected = true;
                } else {
                    connecting = false;
                    heartbeat_due = now;
                    printf("[tcp] connected mono_ms=%lld\n", (long long)now);
                }
            } else if (now >= connect_deadline) {
                fprintf(stderr, "[tcp] connect timeout\n");
                disconnected = true;
            }
        }
        if (fd >= 0 && !connecting && !disconnected) {
            if (now >= next_link_check) {
                next_link_check = now + 100;
                int present = local_address_present(fd);
                if (present == 0) {
                    fprintf(stderr, "[tcp] local interface/address unavailable mono_ms=%lld\n", (long long)now);
                    disconnected = true;
                } else if (present < 0) {
                    perror("local address query");
                    atomic_store(&ctx->failed, true);
                    break;
                }
            }
            if (p[1].revents & (POLLERR | POLLHUP | POLLNVAL)) disconnected = true;
            if (!disconnected && (p[1].revents & POLLIN)) {
                uint8_t byte;
                ssize_t n = recv(fd, &byte, 1, 0);
                /* 原协议单向发送，无上位机回包；出现回包也明确关闭避免误用。 */
                if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                    disconnected = true;
            }
            if (!disconnected && length && (p[1].revents & POLLOUT)) {
                ssize_t n = send(fd, frame + offset, length - offset, MSG_NOSIGNAL);
                if (n > 0) {
                    offset += (size_t)n;
                    if (offset == length) {
                        if (key) {
                            ++ctx->sent;
                            printf("[send-key] seq=%u queue_to_send_ms=%lld mono_ms=%lld\n",
                                   message.sequence, (long long)(now - message.queued_ms),
                                   (long long)now);
                        } else printf("[send-heartbeat] mono_ms=%lld\n", (long long)now);
                        length = offset = 0;
                    }
                } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                    disconnected = true;
            }
            if (length && now >= send_deadline) {
                fprintf(stderr, "[tcp] send stalled for 500ms\n");
                disconnected = true;
            }
        }
        if (disconnected) {
            if (length && key) {
                ++ctx->uncertain;
                fprintf(stderr, "[uncertain] seq=%u delivery unknown; no automatic replay\n",
                        message.sequence);
            }
            close(fd);
            fd = -1;
            length = offset = 0;
            /* 已连接后的故障立即尝试一次；失败的连接保持每秒一次。 */
            if (!connecting) next_connect = now;
            connecting = false;
            printf("[tcp] disconnected mono_ms=%lld\n", (long long)now);
        }
    }
    if (length && key) {
        ++ctx->uncertain;
        fprintf(stderr, "[uncertain] stopping with an unfinished key frame\n");
    }
    if (fd >= 0) close(fd);
    return NULL;
}

#else
#define TRANSPORT_NAME "UDP"
#include "udp_transport.inc"
#endif

/* 读取驱动当前状态；结合 evdev 事件流，避免漏掉采样间的释放/抖动边沿。 */
static int key_down(int fd, bool *down)
{
    unsigned char keys[(KEY_MAX + 8) / 8] = {0};
    if (ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) < 0) return -1;
    *down = (keys[KEY_VOLUMEUP / 8] & (1U << (KEY_VOLUMEUP % 8))) != 0;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 6) {
        fprintf(stderr, "Usage: %s WINDOWS_IP PORT [DEVICE | --synthetic | --heartbeat-only] [SECONDS]\n", argv[0]);
        return 1;
    }
    const char *mode = argc >= 4 ? argv[3] : "/dev/input/event7";
    bool synthetic = strcmp(mode, "--synthetic") == 0;
    bool heartbeat_only = strcmp(mode, "--heartbeat-only") == 0;
    char *end;
    long port = strtol(argv[2], &end, 10);
    if (!*argv[2] || *end || port < 1 || port > 65535 || argc > 5) return 1;
    long seconds = 0;
    if (argc == 5) {
        seconds = strtol(argv[4], &end, 10);
        if (!*argv[4] || *end || seconds < 1 || seconds > 86400) return 1;
    }
    if ((synthetic || heartbeat_only) && !seconds) {
        fprintf(stderr, "Test mode requires duration in seconds\n");
        return 1;
    }
    struct context ctx = {0};
    ctx.peer.sin_family = AF_INET;
    ctx.peer.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, argv[1], &ctx.peer.sin_addr) != 1) return 1;
    atomic_init(&ctx.stop, false);
    atomic_init(&ctx.failed, false);
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct sigaction action = {0};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0)
        return 1;
    int fd = -1, status = 0;
    struct debounce d = {0};
    if (!synthetic && !heartbeat_only) {
        fd = open(mode, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) { perror(mode); return 1; }
        unsigned char bits[(KEY_MAX + 8) / 8] = {0};
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0 ||
            !(bits[KEY_VOLUMEUP / 8] & (1U << (KEY_VOLUMEUP % 8))) ||
            key_down(fd, &d.raw) < 0) {
            fprintf(stderr, "Input device must support key code 115\n");
            close(fd);
            return 1;
        }
        d.stable = d.raw; /* 启动时已按住，须先稳定释放后才计新按下。 */
    }
    ctx.qid = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    if (ctx.qid < 0) { perror("msgget"); if (fd >= 0) close(fd); return 1; }
    ctx.wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (ctx.wake_fd < 0) {
        perror("eventfd"); msgctl(ctx.qid, IPC_RMID, NULL);
        if (fd >= 0) close(fd);
        return 1;
    }
    printf(TRANSPORT_NAME " mode=%s qid=%d debounce=10ms heartbeat=1000ms\n", mode, ctx.qid);
    pthread_t thread;
    int error = pthread_create(&thread, NULL, network_thread, &ctx);
    if (error) {
        fprintf(stderr, "pthread_create: %s\n", strerror(error));
        close(ctx.wake_fd); msgctl(ctx.qid, IPC_RMID, NULL);
        if (fd >= 0) close(fd);
        return 1;
    }
    unsigned int count = 0;
    int64_t started = mono_ms();
    while (!interrupted && !atomic_load(&ctx.failed)) {
        int64_t now = mono_ms();
        if (seconds && now - started >= seconds * 1000) break;
        bool down = false;
        if (synthetic) {
            /* 连续每 200ms 一次合成按下，停止前留 200ms 发送尾部事件。 */
            int64_t elapsed = now - started;
            down = elapsed >= 200 && elapsed < seconds * 1000 - 200 && elapsed % 200 < 50;
        } else if (!heartbeat_only) {
            struct input_event event;
            ssize_t n;
            while ((n = read(fd, &event, sizeof(event))) == (ssize_t)sizeof(event)) {
                if (event.type == EV_SYN && event.code == SYN_DROPPED) {
                    fprintf(stderr, "SYN_DROPPED: cannot certify debounce, stopping\n");
                    status = 1; break;
                }
                if (event.type == EV_KEY && event.code == KEY_VOLUMEUP &&
                    (event.value == 0 || event.value == 1)) {
                    bool value = event.value == 1;
                    /* 先记录所有边沿，不在读取积压事件时认定稳定。 */
                    if (value != d.raw) { d.raw = value; d.changed_ms = mono_ms(); }
                }
            }
            if (status) break;
            if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                fprintf(stderr, "input read failed/short: %zd\n", n); status = 1; break;
            }
            if (key_down(fd, &down) < 0) { perror("EVIOCGKEY"); status = 1; break; }
        }
        if (!heartbeat_only && debounce_sample(&d, down, mono_ms()) && enqueue(&ctx, &count) < 0) {
            status = 1; break;
        }
        struct timespec delay = { .tv_sec = 0, .tv_nsec = 2000000L };
        nanosleep(&delay, NULL); /* 约 2ms 采样；稳定窗口为至少 10ms。 */
    }
    atomic_store(&ctx.stop, true);
    wake_network(&ctx);
    error = pthread_join(thread, NULL);
    if (error) { fprintf(stderr, "pthread_join: %s\n", strerror(error)); return 1; }
    struct msqid_ds info;
    unsigned long pending = 0;
    if (msgctl(ctx.qid, IPC_STAT, &info) == 0) pending = info.msg_qnum;
    else status = 1;
    if (msgctl(ctx.qid, IPC_RMID, NULL) < 0) { perror("IPC_RMID"); status = 1; }
    close(ctx.wake_fd);
    if (fd >= 0) close(fd);
    if (atomic_load(&ctx.failed) || ctx.uncertain || pending || count != ctx.sent) status = 1;
    printf("Summary enqueued=%u socket_sent=%u pending=%lu uncertain=%u status=%s\n",
           count, ctx.sent, pending, ctx.uncertain, status ? "INCOMPLETE" : "OK");
    /* socket_sent 只证明内核接受，原协议没有应用确认，不等于上位机验收。 */
    return status;
}
