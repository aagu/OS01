/*
 * t2_lib.c — the T2 acceptance shared object (/lib/libt2.so).
 *
 * Freestanding, -fPIC, linked with:
 *   -shared -nostdlib --hash-style=sysv -z norelro
 *   -z max-page-size=4096 --pack-dyn-relocs=none
 *   -soname /lib/libt2.so
 *
 * Exports one data symbol and one function symbol; the shapes the
 * T2 contract exercises:
 *   - `t2_value` is referenced from inside the library, so the
 *     default-visibility definition goes through the GOT: the lib's
 *     own .rela.dyn carries a R_X86_64_GLOB_DAT against it.
 *   - `add_fn` is initialized with the address of `add_impl` and is
 *     volatile ON PURPOSE: a plain static pointer is inlined by
 *     clang -O2, the RELATIVE record disappears, and the shape gate
 *     would fail.  With it, the initializer produces the RELATIVE
 *     record t2_add's call actually consumes — if ld.so skipped the
 *     library's RELATIVE pass, add_fn would keep its unrelocated
 *     link-time value and the call would jump to a bogus low address.
 */

long t2_value = 41;

static long add_impl(long a, long b)
{
    return a + b + t2_value;
}

static long (*volatile add_fn)(long, long) = add_impl;

long t2_add(long a, long b)
{
    return add_fn(a, b);
}
