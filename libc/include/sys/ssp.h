#ifndef _SYS_SSP_H
#define _SYS_SSP_H
#include <stdint.h>

/* 每进程 canary：libc/ssp/ssp.c 定义（BSS；libk 构建为空翻译单元），
 * __libc_start_main()（csu.c）每次 exec 播种（spec 2026-09-17 §6.1）。 */
extern unsigned long __stack_chk_guard;

/* -fstack-protector-strong epilogue 在 guard 不匹配时调用。 */
void __stack_chk_fail(void) __attribute__((noreturn));

#endif