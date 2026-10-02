#include <arch/aarch64/boot_direct_map.h>
uint64_t aarch64_read_ttbr1(void)
{
    uint64_t raw;
    __asm__ __volatile__("mrs %0, ttbr1_el1" : "=r"(raw));
    return raw;
}
void aarch64_m1_install_ttbr1(uint64_t pa)
{
    __asm__ __volatile__(
        "dsb ishst\n\tmsr ttbr1_el1, %0\n\tisb\n\ttlbi vmalle1\n\tdsb ish\n\tisb" ::"r"(pa)
        : "memory");
}
void aarch64_m1_flush_all(void)
{
    __asm__ __volatile__("dsb ishst\n\ttlbi vmalle1\n\tdsb ish\n\tisb" ::: "memory");
}

uint64_t aarch64_m1_translation(uint64_t va)
{
    uint64_t par;
    __asm__ __volatile__("at s1e1r, %1\n\tisb\n\tmrs %0, par_el1" : "=r"(par) : "r"(va) : "memory");
    return par;
}
