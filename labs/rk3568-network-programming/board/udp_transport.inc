/* key_network.c 的 UDP 网络线程实现，复用其中 context、队列载荷和单调时钟。
 * 仅 TASK2_UDP 构建包含此文件；不单独编译。无 connect/握手/重连/应用重传。
 */
static void *network_thread(void *argument)
{
    struct context *ctx = argument;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("UDP socket"); atomic_store(&ctx->failed, true); return NULL; }
    int64_t heartbeat_due = mono_ms();
    unsigned int heartbeats = 0, failures = 0, dropped = 0;
    while (!atomic_load(&ctx->stop)) {
        int64_t now = mono_ms();
        uint8_t frame[8];
        size_t length = 0;
        bool key = false;
        struct key_message message = {0};
        if (now >= heartbeat_due) {
            length = encode_frame(frame, false, (uint32_t)time(NULL));
            do { heartbeat_due += 1000; } while (heartbeat_due <= now);
        } else {
            ssize_t n = msgrcv(ctx->qid, &message, MESSAGE_SIZE, 0, IPC_NOWAIT);
            if (n >= 0) {
                if ((size_t)n != MESSAGE_SIZE || message.type != 2) {
                    fprintf(stderr, "Invalid queue payload\n");
                    atomic_store(&ctx->failed, true); break;
                }
                memcpy(frame, message.frame, sizeof(frame));
                length = sizeof(frame);
                key = true;
            } else if (errno != ENOMSG && errno != EINTR) {
                perror("UDP msgrcv"); atomic_store(&ctx->failed, true); break;
            }
        }
        if (length) {
            ssize_t n;
            do {
                n = sendto(fd, frame, length, MSG_NOSIGNAL,
                           (struct sockaddr *)&ctx->peer, sizeof(ctx->peer));
            } while (n < 0 && errno == EINTR && !atomic_load(&ctx->stop));
            now = mono_ms();
            if (n == (ssize_t)length) {
                if (key) {
                    ++ctx->sent;
                    printf("[send-key] seq=%u queue_to_send_ms=%lld mono_ms=%lld\n",
                           message.sequence, (long long)(now - message.queued_ms), (long long)now);
                } else {
                    ++heartbeats;
                    printf("[send-heartbeat] mono_ms=%lld\n", (long long)now);
                }
            } else {
                int error = n < 0 ? errno : EMSGSIZE;
                ++failures;
                if (key) ++dropped;
                fprintf(stderr, "[udp-send-failed] type=%s seq=%u error=%s mono_ms=%lld; no replay\n",
                        key ? "key" : "heartbeat", message.sequence, strerror(error), (long long)now);
                /* 网络不可达/缓冲区满只影响当前帧，不能伪报成功或重放旧事件。 */
                if (error == EBADF || error == EINVAL || error == ENOTSOCK || error == EMSGSIZE) {
                    atomic_store(&ctx->failed, true); break;
                }
            }
            continue; /* 每帧后检查心跳；持续按键也不会饿死心跳。 */
        }
        struct pollfd wake = { .fd = ctx->wake_fd, .events = POLLIN };
        int wait_ms = heartbeat_due > now && heartbeat_due - now < 20 ?
                      (int)(heartbeat_due - now) : 20;
        int rc = poll(&wake, 1, wait_ms);
        if (rc < 0 && errno != EINTR) {
            perror("UDP poll"); atomic_store(&ctx->failed, true); break;
        }
        if (wake.revents & POLLIN) {
            uint64_t value;
            while (read(ctx->wake_fd, &value, sizeof(value)) < 0 && errno == EINTR) {}
        }
    }
    printf("UDP summary heartbeat_socket_sent=%u send_failures=%u key_dropped=%u\n",
           heartbeats, failures, dropped);
    close(fd);
    return NULL;
}
