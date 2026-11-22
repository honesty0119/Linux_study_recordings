/* 协议与消抖公共逻辑：不依赖设备或网络，便于用确定时间序列测试。 */
#ifndef TASK2_CORE_H
#define TASK2_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* raw 为最近采样，stable 为确认状态；changed_ms 使用单调时钟毫秒。 */
struct debounce {
    bool raw, stable;
    int64_t changed_ms;
};

/* 每次状态改变重新计时；按下和释放均需稳定 10ms，仅返回有效按下。 */
static inline bool debounce_sample(struct debounce *d, bool down, int64_t now)
{
    if (down != d->raw) {
        d->raw = down;
        d->changed_ms = now;
    }
    if (d->raw != d->stable && now - d->changed_ms >= 10) {
        d->stable = d->raw;
        return d->stable;
    }
    return false;
}

/* timestamp 为 Unix 秒数；逐字节写大端格式，避免结构体对齐和端序影响。 */
static inline size_t encode_frame(uint8_t *out, bool key, uint32_t timestamp)
{
    size_t offset = key ? 3 : 1;
    size_t size = key ? 8 : 6;
    out[0] = key ? 2 : 1;
    if (key) {
        out[1] = 0; /* 逻辑 KEY0，对应已确认的 Linux KEY_VOLUMEUP。 */
        out[2] = 1;
    }
    for (size_t i = 0; i < 4; ++i)
        out[offset + i] = (uint8_t)(timestamp >> (24 - 8 * i));
    out[size - 1] = 0;
    for (size_t i = 0; i < size - 1; ++i)
        out[size - 1] ^= out[i];
    return size;
}
#endif
