#include <kernel/memory.h>
#include <kernel/printk.h>
#include <stdbool.h>

static uint64_t MemChar(uint64_t val)
{
    return (val >= 0x20 && val < 0x80) ? val : '.';
}

void mem_dump(const void * start, const void * end)
{
    color_printk(BLUE, BLACK, "mem dump from %#016lx to %#016lx\n", start, end);

    uint8_t * p = (uint8_t *)start;

    bool zero_ignore_print = false;

    while (p < (uint8_t *)end)
    {
        uint64_t offset = (uint64_t)p & 0xffffffff;
        if (*(uint64_t *)offset || p + 16 >= (uint8_t *)end) {
            color_printk(YELLOW, BLACK, "%#016lx:\t"
                "%02x %02x %02x %02x  "
                "%02x %02x %02x %02x  "
                "%02x %02x %02x %02x  "
                "%02x %02x %02x %02x  "
                "%c%c%c%c%c%c%c%c"
                "%c%c%c%c%c%c%c%c\n",
                offset,
                p[0x0], p[0x1], p[0x2], p[0x3],
                p[0x4], p[0x5], p[0x6], p[0x7],
                p[0x8], p[0x9], p[0xa], p[0xb],
                p[0xc], p[0xd], p[0xe], p[0xf],
                MemChar(p[0x0]), MemChar(p[0x1]),
                MemChar(p[0x2]), MemChar(p[0x3]),
                MemChar(p[0x4]), MemChar(p[0x5]),
                MemChar(p[0x6]), MemChar(p[0x7]),
                MemChar(p[0x8]), MemChar(p[0x9]),
                MemChar(p[0xa]), MemChar(p[0xb]),
                MemChar(p[0xc]), MemChar(p[0xd]),
                MemChar(p[0xe]), MemChar(p[0xf]));
            zero_ignore_print = false;
        }
        else
        {
            if (!zero_ignore_print)
            {
                color_printk(YELLOW, BLACK, "........\n");
                zero_ignore_print = true;
            }
        }
        
        p += 16;
    }
    
}