/* hosttests/cases/test_lwip_rand.c — LWIP_RAND 调用路径白盒测试
 * （spec 2026-09-17 §7 Layer 1）。
 * 链接生产源 kernel/net/lwip_sys_arch.c（host 编译，-I kernel/include），
 * 以 -Wl,--wrap=get_random_bytes 把生产对象内的 get_random_bytes 调用
 * 重定向到本文件的 __wrap_get_random_bytes。断言：lwip_getrandom_u32()
 * 恰好拉 4 字节并原样返回。 */
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

static int    wrap_calls;
static size_t wrap_len;

void __wrap_get_random_bytes(void *buf, size_t len)
{
    wrap_calls++;
    wrap_len = len;
    /* 0xDEADBEEF, little-endian */
    ((uint8_t *)buf)[0] = 0xEF;
    ((uint8_t *)buf)[1] = 0xBE;
    ((uint8_t *)buf)[2] = 0xAD;
    ((uint8_t *)buf)[3] = 0xDE;
}

extern uint32_t lwip_getrandom_u32(void);

int main(void)
{
    int failed = 0;
    uint32_t v = lwip_getrandom_u32();
    if (wrap_calls != 1) { printf(">>> wrap_calls=%d want 1\n", wrap_calls); failed++; }
    if (wrap_len != 4)   { printf(">>> wrap_len=%zu want 4\n", wrap_len); failed++; }
    if (v != 0xDEADBEEFu) { printf(">>> v=0x%08x want 0xDEADBEEF\n", v); failed++; }
    (void)lwip_getrandom_u32();
    if (wrap_calls != 2) { printf(">>> second call not forwarded\n"); failed++; }
    if (failed) { printf(">>> FAILURES: %d\n", failed); return 1; }
    printf("=== test_lwip_rand: ALL PASSED\n");
    return 0;
}
