// kernel/net/lwip_sys_arch.c — lwIP 随机源适配（spec 2026-09-17 §6.4）。
//
// 只依赖 <random/random.h> + <stdint.h>：内核构建（wildcard net/*.c）与
// hosttests 白盒构建（-I kernel/include + --wrap）共用本文件。
//
// 绝不能用 libc getrandom() wrapper：__is_libk 下恒返回 -1
// （libc/unistd/getrandom.c:7-9），lwIP 轮询它会永久自旋。
#include <random/random.h>
#include <stdint.h>

uint32_t lwip_getrandom_u32(void)
{
    uint32_t v;
    get_random_bytes(&v, sizeof(v));
    return v;
}
