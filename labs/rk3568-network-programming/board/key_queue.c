/*
 * RK3568 按键与 System V 消息队列最小实验。
 * 主线程读取 evdev，只将 KEY_VOLUMEUP 的新按下入队；
 * 消费线程读取队列并打印。Ctrl+C 后停止生产，排空并释放自己的队列。
 * 本阶段复用输入驱动的按键处理，应用只过滤长按重复；
 * 尚未实现独立的 10ms 软件消抖、网络和心跳。
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <time.h>
#include <unistd.h>

/* mtype 必须为正数；msgsnd/msgrcv 的长度不包含此字段。 */
struct message {
    long mtype;
    unsigned int sequence;
    unsigned int code;
};
#define PAYLOAD_SIZE (sizeof(struct message) - sizeof(long))

struct context {
    int qid;
    atomic_bool producer_done;
    atomic_bool consumer_failed;
    unsigned int received; /* 只由消费者写，主线程在 join 后读取。 */
};

static volatile sig_atomic_t interrupted = 0;

static void on_signal(int signal_number)
{
    (void)signal_number;
    interrupted = 1; /* 信号处理器不做 printf、线程或队列操作。 */
}

/* 队列暂时为空时休息 10ms，避免空转；此等待不是按键消抖。 */
static void *consume(void *argument)
{
    struct context *ctx = argument;
    for (;;) {
        struct message msg;
        /*
         * 先读 done，再尝试收消息：若已看到 done，生产者不会再写入。
         * 这样不会在最后一条消息入队的同时误判为空并提前退出。
         */
        bool done = atomic_load(&ctx->producer_done);
        ssize_t n = msgrcv(ctx->qid, &msg, PAYLOAD_SIZE, 0, IPC_NOWAIT);
        if (n >= 0) {
            if ((size_t)n != PAYLOAD_SIZE ||
                msg.sequence != ctx->received + 1 ||
                msg.code != KEY_VOLUMEUP) {
                fprintf(stderr, "Invalid message or sequence\n");
                atomic_store(&ctx->consumer_failed, true);
                break;
            }
            ++ctx->received;
            printf("[consume] seq=%u code=%u\n", msg.sequence, msg.code);
            continue;
        }
        if (errno == EINTR)
            continue;
        if (errno != ENOMSG) {
            perror("msgrcv");
            atomic_store(&ctx->consumer_failed, true);
            break;
        }
        if (done)
            break;
        struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000L };
        nanosleep(&delay, NULL);
    }
    return NULL;
}

/* 按下状态避免重复 value=1；value=2 自动重复不生成新消息。 */
static int handle_event(struct context *ctx, const struct input_event *event,
                        bool *pressed, unsigned int *sent)
{
    if (event->type == EV_SYN && event->code == SYN_DROPPED) {
        fprintf(stderr, "Input events lost; stop and restart after releasing key\n");
        return -1;
    }
    if (event->type != EV_KEY || event->code != KEY_VOLUMEUP)
        return 0;
    if (event->value == 0) {
        *pressed = false;
        return 0;
    }
    if (event->value != 1 || *pressed)
        return 0;
    *pressed = true;
    struct message msg = {0};
    msg.mtype = 1;
    msg.sequence = *sent + 1;
    msg.code = event->code;
    int result;
    do {
        result = msgsnd(ctx->qid, &msg, PAYLOAD_SIZE, IPC_NOWAIT);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        /* 队列满时明确报错并停止，不能静默漏报或无限阻塞采集。 */
        perror("msgsnd (queue full or unavailable)");
        return -1;
    }
    ++*sent;
    printf("[enqueue] seq=%u code=%u\n", msg.sequence, msg.code);
    return 0;
}

int main(int argc, char **argv)
{
    bool self_test = argc == 2 && strcmp(argv[1], "--self-test") == 0;
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [input-device | --self-test]\n", argv[0]);
        return 1;
    }
    const char *device = argc == 2 ? argv[1] : "/dev/input/event7";
    int fd = -1;
    int status = 0;
    bool pressed = false;
    unsigned int sent = 0;
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct sigaction action = {0};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0) {
        perror("sigaction");
        return 1;
    }

    if (!self_test) {
        fd = open(device, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            perror(device);
            return 1;
        }
        /* 启动时已按住的键不算新按下，先等待松开。 */
        unsigned char keys[(KEY_MAX + 8) / 8] = {0};
        if (ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) < 0) {
            perror("EVIOCGKEY");
            close(fd);
            return 1;
        }
        pressed = (keys[KEY_VOLUMEUP / 8] &
                   (1U << (KEY_VOLUMEUP % 8))) != 0;
    }

    struct context ctx = {0};
    atomic_init(&ctx.producer_done, false);
    atomic_init(&ctx.consumer_failed, false);
    /* 每次运行创建独立队列，不连接或清理其他程序的队列。 */
    ctx.qid = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    if (ctx.qid < 0) {
        perror("msgget");
        if (fd >= 0)
            close(fd);
        return 1;
    }
    printf("qid=%d payload=%zu bytes mode=%s\n",
           ctx.qid, PAYLOAD_SIZE, self_test ? "self-test" : device);
    pthread_t thread;
    int err = pthread_create(&thread, NULL, consume, &ctx);
    if (err != 0) {
        fprintf(stderr, "pthread_create: %s\n", strerror(err));
        if (msgctl(ctx.qid, IPC_RMID, NULL) < 0)
            perror("msgctl cleanup");
        if (fd >= 0)
            close(fd);
        return 1;
    }

    if (self_test) {
        /*
         * 合成 4 次按下，混入自动重复、重复按下和无关按键；
         * 使用真实消息队列与消费线程，但不能代替板端硬件验收。
         */
        const int values[] = {1, 2, 1, 0, 1, 0, 1, 0, 1, 2, 2, 0};
        struct input_event event = {0};
        event.type = EV_KEY;
        event.code = KEY_MENU;
        event.value = 1;
        status = handle_event(&ctx, &event, &pressed, &sent);
        event.code = KEY_VOLUMEUP;
        for (size_t i = 0; i < sizeof(values) / sizeof(values[0]) &&
                           status == 0; ++i) {
            event.value = values[i];
            status = handle_event(&ctx, &event, &pressed, &sent);
        }
    } else {
        printf("Watch code=115; press user key, Ctrl+C to stop\n");
        while (!interrupted && !atomic_load(&ctx.consumer_failed)) {
            struct pollfd input = { .fd = fd, .events = POLLIN };
            int ready = poll(&input, 1, 200);
            if (ready < 0) {
                if (errno == EINTR)
                    continue;
                perror("poll");
                status = 1;
                break;
            }
            if (ready == 0)
                continue;
            if (input.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                fprintf(stderr, "Input device unavailable\n");
                status = 1;
                break;
            }
            if (!(input.revents & POLLIN))
                continue;
            struct input_event event;
            ssize_t n = read(fd, &event, sizeof(event));
            if (n < 0 && (errno == EAGAIN || errno == EINTR))
                continue;
            if (n != (ssize_t)sizeof(event)) {
                fprintf(stderr, "Incomplete input event: %zd bytes\n", n);
                status = 1;
                break;
            }
            if (handle_event(&ctx, &event, &pressed, &sent) < 0) {
                status = 1;
                break;
            }
        }
    }

    atomic_store(&ctx.producer_done, true);
    err = pthread_join(thread, NULL);
    if (err != 0) {
        fprintf(stderr, "pthread_join: %s\n", strerror(err));
        /* 不再访问可能仍由工作线程写入的字段。 */
        msgctl(ctx.qid, IPC_RMID, NULL);
        return 1;
    }
    if (atomic_load(&ctx.consumer_failed) || sent != ctx.received ||
        (self_test && sent != 4))
        status = 1;
    if (msgctl(ctx.qid, IPC_RMID, NULL) < 0) {
        perror("msgctl cleanup");
        status = 1;
    }
    if (fd >= 0)
        close(fd);
    printf("Result: sent=%u received=%u %s\n", sent, ctx.received,
           status == 0 ? "PASS" : "FAIL");
    return status == 0 ? 0 : 1;
}
