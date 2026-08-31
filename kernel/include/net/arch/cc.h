// kernel/include/net/arch/cc.h — lwIP platform compiler abstraction for OS01
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

// ═══ Avoid pulling in OS01 userspace headers ═══
// <stdint.h> from the sysroot pulls in <sys/types.h> which defines
// ssize_t = long.  lwIP arch.h defines ssize_t = int (when SSIZE_MAX
// is not set).  The two conflict.  Define integer types here directly
// to bypass the whole include chain.
//
// Note: the kernel's clang target (x86_64-unknown-none with
// -ffreestanding) does provide built-in integer types via compiler
// builtins — we typedef from those.
//
// The widths below deliberately mirror OS01's libc/include/stdint.h
// (e.g. uint64_t = unsigned long long), NOT canonical Clang stdint
// ownership — on LP64 Clang would typedef uint64_t = unsigned long.
// Keeping kernel and userspace ABI identical prevents lwIP structs
// crossing the user/kern boundary (sockets, timevals, msghdr) from
// aliasing differently at the same C type name.
//
// future-review pointer: a rewrite of libc/include/stdint.h must
// re-review this local typedef block AND the ssize_t rationale above
// (cc.h:5-23) — this block owns the kernel-side ABI for these names
// until OS01 gains a clean freestanding/hosted stdint split.

typedef unsigned char      uint8_t;
typedef signed char        int8_t;
typedef unsigned short     uint16_t;
typedef signed short       int16_t;
typedef unsigned int       uint32_t;
typedef signed int         int32_t;
typedef unsigned long long uint64_t;
typedef signed long long   int64_t;
typedef unsigned long      uintptr_t;

#include <kernel/log.h>
#include <device/timer.h>  // jiffies (for LWIP_RAND)
#include <errno.h>          // EIO, EINVAL, etc.
// EWOULDBLOCK = EAGAIN (same as Linux)
#define EWOULDBLOCK EAGAIN

// ── lwIP basic types ──────────────────────────────────────────
typedef uint8_t   u8_t;
typedef int8_t    s8_t;
typedef uint16_t  u16_t;
typedef int16_t   s16_t;
typedef uint32_t  u32_t;
typedef int32_t   s32_t;

typedef uintptr_t mem_ptr_t;

// ── Diagnostic / debug output ──────────────────────────────────
#define LWIP_PLATFORM_DIAG(x) do { log_info x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { \
    log_err("lwIP assert: %s at %s:%d\n", x, __FILE__, __LINE__); \
} while (0)

// ── Byte order (x86_64 is little-endian) ──────────────────────
// Prevent lwIP arch.h from including OS headers — we define all
// integer types above.  OS01's <inttypes.h> and <stdint.h> pull in
// <sys/types.h> which defines ssize_t = long, conflicting with
// lwIP's ssize_t = int.
#define LWIP_NO_STDINT_H   1
#define LWIP_NO_INTTYPES_H 1

// ── (sn)printf format macros ──────────────────────────────────
// LWIP_NO_INTTYPES_H=1 above tells lwIP not to pull in <inttypes.h>,
// so arch.h's auto-definition of X8_F/U16_F/S16_F/... never runs.
// Define them here (matching OS01's <inttypes.h> values).
#define X8_F   "02" "x"
#define U16_F  "u"
#define S16_F  "d"
#define X16_F  "x"
#define U32_F  "u"
#define S32_F  "d"
#define X32_F  "x"
#define U64_F  "llu"
#define S64_F  "lld"
#define X64_F  "llx"
// Define SSIZE_MAX so lwIP arch.h takes the #ifdef SSIZE_MAX path
// (no-ops when LWIP_NO_UNISTD_H=1) instead of typedef'ing ssize_t.
#define SSIZE_MAX 0x7FFFFFFFFFFFFFFFULL
// Provide ssize_t ourselves since we blocked all OS headers.
typedef long ssize_t;
#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

// ── Random (used by DNS for TXID) ─────────────────────────────
// Simple LCG for DNS TXID — sufficient for a hobby OS
#define LWIP_RAND() ((u32_t)(jiffies * 1103515245 + 12345))

// ── Packed struct macros ──────────────────────────────────────
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x

// ── Platform headers ──────────────────────────────────────────
#include <stddef.h>
#include <string.h>

#endif // LWIP_ARCH_CC_H
