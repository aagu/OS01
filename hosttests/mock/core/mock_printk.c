/* mock_printk.c — color_printk/serial_printk capture with 4KB ring buffer. */
#include "mock_core.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define MOCK_PRINTK_BUF_SIZE 4096

extern int _mock_record_call(const char *symbol);

static char g_printk_ring[MOCK_PRINTK_BUF_SIZE];
static size_t g_printk_head = 0;
static char g_printk_last[MOCK_PRINTK_BUF_SIZE];

void _mock_printk_reset(void)
{
    memset(g_printk_ring, 0, sizeof(g_printk_ring));
    memset(g_printk_last, 0, sizeof(g_printk_last));
    g_printk_head = 0;
}

static void ring_append(const char *msg, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        g_printk_ring[g_printk_head] = msg[i];
        g_printk_head = (g_printk_head + 1) % MOCK_PRINTK_BUF_SIZE;
    }
    g_printk_ring[g_printk_head] = '\0';
    g_printk_head = (g_printk_head + 1) % MOCK_PRINTK_BUF_SIZE;
}

static int record_printk(const char *symbol, const char *fmt, va_list args)
{
    (void)_mock_record_call(symbol);
    int n = vsnprintf(g_printk_last, sizeof(g_printk_last), fmt, args);
    if (n < 0) {
        g_printk_last[0] = '\0';
        return n;
    }
    size_t stored = (size_t)n;
    if (stored >= sizeof(g_printk_last)) {
        stored = sizeof(g_printk_last) - 1;
    }
    ring_append(g_printk_last, stored);
    return n;
}

int color_printk(unsigned int FRcolor, unsigned int BKcolor,
                 const char *fmt, ...)
{
    (void)FRcolor;
    (void)BKcolor;
    va_list args;
    va_start(args, fmt);
    int ret = record_printk("color_printk", fmt, args);
    va_end(args);
    return ret;
}

int serial_printk(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int ret = record_printk("serial_printk", fmt, args);
    va_end(args);
    return ret;
}

const char *mock_printk_last(void)
{
    return g_printk_last;
}
