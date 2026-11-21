/* @os01-test
 * id: test_exception_trace
 * layer: host
 * subsystem: intr
 * timing: strict
 * tier: pr
 * scenarios: []
 */
#include <test_framework.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <arch/regs.h>
#include <arch/unwind.h>
#include <core/trace.h>
#include <core/trace_symbols.h>

/* Captured early UART output */
static char g_uart_buf[8192];
static size_t g_uart_len = 0;

void arch_early_putc(char c)
{
    if (g_uart_len + 1 < sizeof(g_uart_buf)) {
        g_uart_buf[g_uart_len++] = c;
        g_uart_buf[g_uart_len] = '\0';
    }
}

void arch_early_puts(const char *s)
{
    while (*s) {
        arch_early_putc(*s++);
    }
}

/* Mock unwind state */
static struct unwind_cursor g_init_cursor;
static bool g_last_allow_walk = false;
static int g_read_calls = 0;

struct mock_frame_entry {
    uintptr_t fp;
    bool ok;
    struct unwind_frame frame;
};

static struct mock_frame_entry g_mock_frames[16];
static size_t g_mock_frame_count = 0;

static uintptr_t g_text_start = UINT64_C(0xffff800000100000);
static uintptr_t g_text_end   = UINT64_C(0xffff800000200000);

struct mock_sym_entry {
    uintptr_t addr;
    const char *name;
    uint64_t offset;
};

static struct mock_sym_entry g_mock_syms[16];
static size_t g_mock_sym_count = 0;

static void reset_mocks(void)
{
    g_uart_len = 0;
    g_uart_buf[0] = '\0';
    memset(&g_init_cursor, 0, sizeof(g_init_cursor));
    g_last_allow_walk = false;
    g_read_calls = 0;
    g_mock_frame_count = 0;
    g_mock_sym_count = 0;
}

void arch_unwind_init(struct unwind_cursor *cursor, const struct pt_regs *regs,
                      bool allow_walk)
{
    (void)regs;
    g_last_allow_walk = allow_walk;
    *cursor = g_init_cursor;
}

bool arch_unwind_read_frame(uintptr_t fp, struct unwind_frame *out)
{
    g_read_calls++;
    for (size_t i = 0; i < g_mock_frame_count; i++) {
        if (g_mock_frames[i].fp == fp) {
            if (!g_mock_frames[i].ok) {
                return false;
            }
            *out = g_mock_frames[i].frame;
            return true;
        }
    }
    return false;
}

bool arch_unwind_return_query_pc(uintptr_t ret, uintptr_t *query_pc)
{
    if (ret == 0) {
        return false;
    }
    *query_pc = ret - 1;
    return true;
}

bool trace_kernel_text_contains(uintptr_t pc)
{
    return pc >= g_text_start && pc < g_text_end;
}

bool trace_symbol_lookup(uintptr_t query_pc, const char **name, uint64_t *offset)
{
    for (size_t i = 0; i < g_mock_sym_count; i++) {
        if (g_mock_syms[i].addr == query_pc) {
            if (name) {
                *name = g_mock_syms[i].name;
            }
            if (offset) {
                *offset = g_mock_syms[i].offset;
            }
            return true;
        }
    }
    return false;
}

static size_t count_substr(const char *haystack, const char *needle)
{
    size_t count = 0;
    size_t nlen = strlen(needle);
    const char *p = haystack;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += nlen;
    }
    return count;
}

TEST_FUNC(test_trace_four_frames_and_end_marker)
{
    reset_mocks();
    struct pt_regs regs;
    memset(&regs, 0, sizeof(regs));

    uintptr_t stack_low  = UINT64_C(0xffff800010000000);
    uintptr_t stack_high = UINT64_C(0xffff800010008000);
    uintptr_t fp0 = stack_low + 0x100;
    uintptr_t fp1 = stack_low + 0x120;
    uintptr_t fp2 = stack_low + 0x140;
    uintptr_t fp3 = stack_low + 0x160;

    g_init_cursor = (struct unwind_cursor){
        .pc = g_text_start + 0x100,
        .sp = stack_low + 0x80,
        .fp = fp0,
        .stack_low = stack_low,
        .stack_high = stack_high,
        .frame_align = 8,
        .kernel_origin = true,
    };

    g_mock_frames[0] = (struct mock_frame_entry){ fp0, true, { fp1, g_text_start + 0x201 } };
    g_mock_frames[1] = (struct mock_frame_entry){ fp1, true, { fp2, g_text_start + 0x301 } };
    g_mock_frames[2] = (struct mock_frame_entry){ fp2, true, { fp3, g_text_start + 0x401 } };
    g_mock_frames[3] = (struct mock_frame_entry){ fp3, true, { 0,   g_text_start + 0x501 } };
    g_mock_frame_count = 4;

    g_mock_syms[0] = (struct mock_sym_entry){ g_text_start + 0x100, "leaf_fn", 0x10 };
    g_mock_syms[1] = (struct mock_sym_entry){ g_text_start + 0x200, "caller_1", 0x20 };
    g_mock_syms[2] = (struct mock_sym_entry){ g_text_start + 0x300, "caller_2", 0x30 };
    g_mock_syms[3] = (struct mock_sym_entry){ g_text_start + 0x400, "caller_3", 0x40 };
    g_mock_sym_count = 4;

    trace_exception(&regs, TRACE_FULL);

    assert_true(g_last_allow_walk);
    assert_eq(3, g_read_calls); /* 1st frame is PC, plus 3 frame reads = 4 total */
    assert_eq(4, count_substr(g_uart_buf, "+---> address:"));
    assert_true(strstr(g_uart_buf, "====================== Kernel Stack Backtrace ======================\n") != NULL);
    assert_true(strstr(g_uart_buf, "function:leaf_fn") != NULL);
    assert_true(strstr(g_uart_buf, "function:caller_1") != NULL);
    assert_true(strstr(g_uart_buf, "function:caller_2") != NULL);
    assert_true(strstr(g_uart_buf, "function:caller_3") != NULL);
    assert_true(strstr(g_uart_buf, "====================== End Kernel Stack Backtrace ======================\n") != NULL);
}

TEST_FUNC(test_trace_pc_only_and_user_origin_zero_reads)
{
    struct pt_regs regs;
    memset(&regs, 0, sizeof(regs));

    /* PC-only */
    reset_mocks();
    g_init_cursor = (struct unwind_cursor){
        .pc = g_text_start + 0x100,
        .sp = UINT64_C(0xffff800010000080),
        .fp = UINT64_C(0xffff800010000100),
        .stack_low = UINT64_C(0xffff800010000000),
        .stack_high = UINT64_C(0xffff800010008000),
        .frame_align = 8,
        .kernel_origin = true,
    };
    g_mock_syms[0] = (struct mock_sym_entry){ g_text_start + 0x100, "leaf_fn", 0x10 };
    g_mock_sym_count = 1;

    trace_exception(&regs, TRACE_PC_ONLY);
    assert_false(g_last_allow_walk);
    assert_eq(0, g_read_calls);
    assert_eq(1, count_substr(g_uart_buf, "+---> address:"));
    assert_true(strstr(g_uart_buf, "function:leaf_fn") != NULL);
    assert_true(strstr(g_uart_buf, "====================== End Kernel Stack Backtrace ======================\n") != NULL);

    /* User/unknown origin: 0 reads, numeric PC only (no symbol lookup) */
    reset_mocks();
    g_init_cursor = (struct unwind_cursor){
        .pc = g_text_start + 0x100,
        .kernel_origin = false,
    };
    g_mock_syms[0] = (struct mock_sym_entry){ g_text_start + 0x100, "leaf_fn", 0x10 };
    g_mock_sym_count = 1;

    trace_exception(&regs, TRACE_FULL);
    assert_eq(0, g_read_calls);
    assert_eq(1, count_substr(g_uart_buf, "+---> address:"));
    assert_true(strstr(g_uart_buf, "(no symbol)") != NULL);
    assert_true(strstr(g_uart_buf, "function:leaf_fn") == NULL);
}

TEST_FUNC(test_trace_next_fp_zero_still_prints_return_frame)
{
    reset_mocks();
    struct pt_regs regs;
    memset(&regs, 0, sizeof(regs));

    uintptr_t stack_low  = UINT64_C(0xffff800010000000);
    uintptr_t stack_high = UINT64_C(0xffff800010008000);
    uintptr_t fp0 = stack_low + 0x100;

    g_init_cursor = (struct unwind_cursor){
        .pc = g_text_start + 0x100,
        .sp = stack_low + 0x80,
        .fp = fp0,
        .stack_low = stack_low,
        .stack_high = stack_high,
        .frame_align = 8,
        .kernel_origin = true,
    };
    g_mock_frames[0] = (struct mock_frame_entry){ fp0, true, { 0, g_text_start + 0x201 } };
    g_mock_frame_count = 1;
    g_mock_syms[0] = (struct mock_sym_entry){ g_text_start + 0x100, "leaf_fn", 0 };
    g_mock_syms[1] = (struct mock_sym_entry){ g_text_start + 0x200, "entry_fn", 0 };
    g_mock_sym_count = 2;

    trace_exception(&regs, TRACE_FULL);
    assert_eq(1, g_read_calls);
    assert_eq(2, count_substr(g_uart_buf, "+---> address:"));
    assert_true(strstr(g_uart_buf, "function:entry_fn") != NULL);
}

TEST_FUNC(test_trace_rejects_bad_fp_and_out_of_text_return_pc)
{
    struct pt_regs regs;
    memset(&regs, 0, sizeof(regs));
    uintptr_t stack_low  = UINT64_C(0xffff800010000000);
    uintptr_t stack_high = UINT64_C(0xffff800010008000);

    /* Misaligned FP, fp < sp, crossing upper bound, near UINTPTR_MAX */
    uintptr_t bad_fps[] = {
        stack_low + 0x104,
        stack_low + 0x40,
        stack_high - 8,
        UINTPTR_MAX - 4,
    };
    for (size_t i = 0; i < sizeof(bad_fps) / sizeof(bad_fps[0]); i++) {
        reset_mocks();
        g_init_cursor = (struct unwind_cursor){
            .pc = g_text_start + 0x100,
            .sp = stack_low + 0x80,
            .fp = bad_fps[i],
            .stack_low = stack_low,
            .stack_high = stack_high,
            .frame_align = 8,
            .kernel_origin = true,
        };
        trace_exception(&regs, TRACE_FULL);
        assert_eq(0, g_read_calls);
        assert_eq(1, count_substr(g_uart_buf, "+---> address:"));
    }

    /* Loop / overlapping next_fp (next_fp < fp + 16) truncates before 2nd read */
    uintptr_t fp0 = stack_low + 0x100;
    uintptr_t overlap_next[] = { fp0, fp0 + 8, fp0 - 16 };
    for (size_t i = 0; i < sizeof(overlap_next) / sizeof(overlap_next[0]); i++) {
        reset_mocks();
        g_init_cursor = (struct unwind_cursor){
            .pc = g_text_start + 0x100,
            .sp = stack_low + 0x80,
            .fp = fp0,
            .stack_low = stack_low,
            .stack_high = stack_high,
            .frame_align = 8,
            .kernel_origin = true,
        };
        g_mock_frames[0] = (struct mock_frame_entry){ fp0, true, { overlap_next[i], g_text_start + 0x201 } };
        g_mock_frame_count = 1;
        trace_exception(&regs, TRACE_FULL);
        assert_eq(1, g_read_calls);
        assert_eq(2, count_substr(g_uart_buf, "+---> address:"));
    }

    /* Subsequent return_pc outside kernel text: does NOT print bogus frame */
    reset_mocks();
    g_init_cursor = (struct unwind_cursor){
        .pc = g_text_start + 0x100,
        .sp = stack_low + 0x80,
        .fp = fp0,
        .stack_low = stack_low,
        .stack_high = stack_high,
        .frame_align = 8,
        .kernel_origin = true,
    };
    g_mock_frames[0] = (struct mock_frame_entry){ fp0, true, { fp0 + 0x20, UINT64_C(0xdeadbeef0000) } };
    g_mock_frame_count = 1;
    trace_exception(&regs, TRACE_FULL);
    assert_eq(1, g_read_calls);
    assert_eq(1, count_substr(g_uart_buf, "+---> address:"));
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_trace_four_frames_and_end_marker),
    TEST_ENTRY(test_trace_pc_only_and_user_origin_zero_reads),
    TEST_ENTRY(test_trace_next_fp_zero_still_prints_return_frame),
    TEST_ENTRY(test_trace_rejects_bad_fp_and_out_of_text_return_pc),
TEST_LIST_END

int main(void)
{
    TEST_SUITE("exception-trace");
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
