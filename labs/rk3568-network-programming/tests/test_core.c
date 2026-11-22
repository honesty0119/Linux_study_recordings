/* 确定性测试：协议黄金字节、10ms 边界、抖动、稳定释放与启动时长按。 */
#include "../board/task2_core.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(void)
{
    int checks = 0;
    uint8_t frame[8];
    const uint8_t heartbeat[] = {1, 0x12, 0x34, 0x56, 0x78, 0x09};
    const uint8_t key[] = {2, 0, 1, 0x12, 0x34, 0x56, 0x78, 0x0b};
    CHECK(encode_frame(frame, false, 0x12345678) == 6);
    CHECK(memcmp(frame, heartbeat, 6) == 0);
    CHECK(encode_frame(frame, true, 0x12345678) == 8);
    CHECK(memcmp(frame, key, 8) == 0);
    struct debounce d = {0};
    CHECK(!debounce_sample(&d, true, 100));
    CHECK(!debounce_sample(&d, true, 109));
    CHECK(debounce_sample(&d, true, 110));
    CHECK(!debounce_sample(&d, true, 1000)); /* 长按不重复 */
    CHECK(!debounce_sample(&d, false, 1001));
    CHECK(!debounce_sample(&d, true, 1005)); /* 释放抖动不重新触发 */
    CHECK(!debounce_sample(&d, true, 1015));
    CHECK(!debounce_sample(&d, false, 1020));
    CHECK(!debounce_sample(&d, false, 1030));
    CHECK(!d.stable);
    CHECK(!debounce_sample(&d, true, 1040));
    CHECK(!debounce_sample(&d, false, 1045));
    CHECK(!debounce_sample(&d, true, 1048));
    CHECK(!debounce_sample(&d, true, 1057));
    CHECK(debounce_sample(&d, true, 1058));
    struct debounce held = { .raw = true, .stable = true };
    CHECK(!debounce_sample(&held, true, 5000));
    CHECK(!debounce_sample(&held, false, 5001));
    CHECK(!debounce_sample(&held, false, 5011));
    CHECK(!debounce_sample(&held, true, 5012));
    CHECK(debounce_sample(&held, true, 5022));
    printf("core: %d checks PASS\n", checks);
    return 0;
}
