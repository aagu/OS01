/* mock_core.c — Shared core mock knobs and log/panic stubs for opt-in hosttests. */
#include "mock_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOCK_MAX_SYMBOLS 32

typedef struct {
    const char *symbol;
    long calls;
    int fail_remaining;
} mock_knob_entry_t;

static mock_knob_entry_t g_knobs[MOCK_MAX_SYMBOLS];
static int g_knob_count = 0;

extern void _mock_printk_reset(void);

static mock_knob_entry_t *find_or_add_knob(const char *symbol)
{
    if (!symbol) {
        return NULL;
    }
    for (int i = 0; i < g_knob_count; i++) {
        if (strcmp(g_knobs[i].symbol, symbol) == 0) {
            return &g_knobs[i];
        }
    }
    if (g_knob_count >= MOCK_MAX_SYMBOLS) {
        return NULL;
    }
    mock_knob_entry_t *e = &g_knobs[g_knob_count++];
    e->symbol = symbol;
    e->calls = 0;
    e->fail_remaining = 0;
    return e;
}

int _mock_record_call(const char *symbol)
{
    mock_knob_entry_t *e = find_or_add_knob(symbol);
    if (!e) {
        return 0;
    }
    e->calls++;
    if (e->fail_remaining > 0) {
        e->fail_remaining--;
        return 1;
    }
    return 0;
}

void mock_reset_all(void)
{
    for (int i = 0; i < g_knob_count; i++) {
        g_knobs[i].calls = 0;
        g_knobs[i].fail_remaining = 0;
    }
    _mock_printk_reset();
}

void mock_fail_next(const char *symbol, int n)
{
    mock_knob_entry_t *e = find_or_add_knob(symbol);
    if (!e) {
        return;
    }
    e->fail_remaining = n > 0 ? n : 0;
}

long mock_calls(const char *symbol)
{
    if (!symbol) {
        return 0;
    }
    for (int i = 0; i < g_knob_count; i++) {
        if (strcmp(g_knobs[i].symbol, symbol) == 0) {
            return g_knobs[i].calls;
        }
    }
    return 0;
}

/* ── Log no-op cluster ───────────────────────────────────────────────── */
int g_log_level = 3;

void _log_write(int level, const char *fmt, ...)
{
    (void)level;
    (void)fmt;
}

void _log_writev(int level, const char *fmt, va_list args)
{
    (void)level;
    (void)fmt;
    (void)args;
}

void _log_err_impl(const char *fmt, ...)
{
    (void)fmt;
}

void _log_warn_impl(const char *fmt, ...)
{
    (void)fmt;
}

void _log_info_impl(const char *fmt, ...)
{
    (void)fmt;
}

void log_set_level(int level)
{
    g_log_level = level;
}

int log_get_level(void)
{
    return g_log_level;
}

/* ── Panic stub ──────────────────────────────────────────────────────── */
void kpanic(const char *msg, ...)
{
    (void)_mock_record_call("kpanic");
    va_list args;
    va_start(args, msg);
    vfprintf(stderr, msg, args);
    va_end(args);
    fprintf(stderr, "\nPANIC\n");
    exit(1);
}
