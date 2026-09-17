// user/canary_smash.c — SSP 行为 probe（spec 2026-09-17 Task 1）。
// 帧内 16B volatile 数组故意写越界 48 字节，覆盖该帧 canary；
// -fstack-protector-strong 的 epilogue 必须在 ret 前调 __stack_chk_fail
// → raise(SIGABRT) → 内核 SIG_DFL 致命分发 do_exit(6)（trap.c:838）。
// RED 形态（无 SSP 基线）：corrupt 局部内存后正常 return → exit 0。
#include <stdint.h>

__attribute__((noinline))
static void smash(void)
{
    volatile char buf[16];
    for (int i = 0; i < 64; i++)       /* buf[16..63] 覆盖 canary 及以上 */
        buf[i] = (char)(0x41 + (i & 0x0F));
    buf[0] = 'X';                       /* volatile 写不可被优化掉 */
}

int main(void)
{
    smash();
    return 0;                           /* SSP 下不可达 */
}
