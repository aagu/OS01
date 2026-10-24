/* Real x86 IRQ backend/adapters/business; spies stop only hardware or fatal paths. */
#include <test_framework.h>
#include <setjmp.h>
#include <string.h>
#include <arch/intr.h>
#include <arch/x86_64/intr_decode.h>
#include <arch/x86_64/intr_dispatch.h>
static jmp_buf bad_jb;
static int bad_calls;
__attribute__((noreturn)) void arch_intr_bad_event(const struct intr_event *e,
                                                   pt_regs_t *r) {
    bad_calls++;
    longjmp(bad_jb, 1);
}
void x86_handle_debug(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_nmi(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_int3(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_overflow(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_bounds(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_dev_not_available(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_double_fault(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_coprocessor_segment_overrun(pt_regs_t *r, uint64_t e) {
    __builtin_trap();
}
void x86_handle_invalid_TSS(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_segment_not_present(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_stack_segment_fault(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_x87_FPU_error(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_alignment_check(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_machine_check(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_SIMD_exception(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_virtualization_exception(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_divide_error(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_undefined_opcode(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_general_protection(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_page_fault(pt_regs_t *r, uint64_t e, uint64_t a) { __builtin_trap(); }

#include <arch/irq.h>
#include <intr/interrupt.h>
#include <intr/pic.h>
#include <intr/vector_dispatch.h>
extern void generic_intr_dispatch(pt_regs_t *, uint64_t);
void ipi_init(void);
void irq_test_cpu(unsigned);
uint64_t irq_test_need(void), irq_test_watchdog(void), irq_test_gen(void);
irq_desc_t irq_table[MAX_GSI];
int g_log_level = 0;
void _log_info_impl(const char *f, ...) { __builtin_trap(); }
void _log_err_impl(const char *f, ...) { __builtin_trap(); }
static unsigned disabled, eois, ticks, accounts, softirqs, flushes;
static uint64_t gen_at_eoi, need_at_eoi;
static unsigned trace[16], trace_n;
static uint64_t seen_vector, seen_param;
static pt_regs_t *seen_regs;
static void record(unsigned n) {
    if (trace_n < 16)
        trace[trace_n++] = n;
}
void arch_local_irq_disable(void) {
    disabled++;
    record(1);
}
void arch_outb(uint16_t p, uint8_t v) {
    assert_eq(0x20, v);
    record(p);
}
uint8_t arch_inb(uint16_t p) { return 0; }
void lapic_eoi(void) {
    eois++;
    gen_at_eoi = irq_test_gen();
    need_at_eoi = irq_test_need();
    record(9);
}
void tick_handler(void) {
    ticks++;
    record(3);
}
void sched_account_tick(void) {
    accounts++;
    record(4);
}
void set_softirq_status(uint64_t s) {
    softirqs++;
    record(5);
}
void irq_test_flush_tlb(void) {
    flushes++;
    record(6);
}
static void handler(uint64_t v, uint64_t p, pt_regs_t *r) {
    seen_vector = v;
    seen_param = p;
    seen_regs = r;
    record(2);
}
static void ack(uint64_t v) {
    assert_eq(seen_vector, v);
    record(7);
}
static hw_int_controller_t controller = {.ack = ack};
static void reset(void) {
    bad_calls = disabled = eois = ticks = accounts = softirqs = flushes = trace_n = 0;
    seen_vector = seen_param = 0;
    seen_regs = 0;
    memset(irq_table, 0, sizeof irq_table);
    memset(intr_handler_table, 0, sizeof(void *) * 256);
    memset(intr_handler_param, 0, sizeof(void *) * 256);
    irq_test_cpu(0);
}
static void expect_bad(struct intr_event *, pt_regs_t *);
static void enter(pt_regs_t *r, uint64_t v) {
    if (v >= 0x20 && v <= 0x37)
        arch_irq_dispatch(r, v);
    else if (v == 0x40 || v == 0x41)
        generic_intr_dispatch(r, v);
    else
        x86_irq_entry(r, v);
}
TEST_FUNC(all_28_irq_vectors_both_origins) {
    static const uint64_t vectors[] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
                                       0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d,
                                       0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34,
                                       0x35, 0x36, 0x37, 0x38, 0x40, 0x41, 0xff};
    for (unsigned cpl = 0; cpl <= 3; cpl += 3)
        for (unsigned i = 0; i < 28; i++) {
            uint64_t v = vectors[i];
            pt_regs_t r = {.cs = 8 + cpl};
            struct intr_event e;
            reset();
            x86_intr_decode_irq(&e, &r, v);
            assert_eq(INTR_IRQ, e.kind);
            assert_eq(cpl ? INTR_ORIGIN_USER : INTR_ORIGIN_KERNEL, e.origin);
            assert_eq(INTR_REASON_OTHER, e.reason);
            assert_eq(INTR_ACCESS_UNKNOWN, e.access);
            assert_false(e.fault_addr_valid);
            assert_eq(0, e.fault_addr);
            assert_eq(v, e.native_vector);
            assert_eq(0, e.native_status);
            assert_eq(0, e.native_address);
            if (v <= 0x37) {
                irq_table[v - 0x20].handler = handler;
                irq_table[v - 0x20].parameter = 0x123456789abcdef0ULL;
                irq_table[v - 0x20].controller = &controller;
            }
            if (v == 0x40 || v == 0x41) {
                intr_handler_table[v] = handler;
                intr_handler_param[v] = (void *)0xfedcba9876543210ULL;
            }
            intr_handler_table[0xff] = handler;
            if (!setjmp(bad_jb))
                enter(&r, v);
            assert_eq(0, bad_calls);
            if (v <= 0x37) {
                assert_eq(3, trace_n);
                assert_eq(1, trace[0]);
                assert_eq(2, trace[1]);
                assert_eq(7, trace[2]);
                assert_eq(0x123456789abcdef0ULL, seen_param);
            }
            if (v == 0x40 || v == 0x41) {
                assert_eq(1, trace_n);
                assert_eq(2, trace[0]);
                assert_eq(0xfedcba9876543210ULL, seen_param);
            }
            if (v <= 0x37 || v == 0x40 || v == 0x41) {
                assert_eq(v, seen_vector);
                assert_true(seen_regs == &r);
            }
            assert_eq(v == 0x38 ? 1 : 0, eois);
            if (v == 0xff) {
                assert_eq(0, trace_n);
                assert_eq(0, disabled);
            }
            for (unsigned unknown = 1; unknown <= 2; unknown++) {
                reset();
                r.cs = unknown;
                x86_intr_decode_irq(&e, &r, v);
                assert_eq(INTR_ORIGIN_UNKNOWN, e.origin);
                expect_bad(&e, &r);
            }
        }
}
TEST_FUNC(device_controller_ack_ownership) {
    pt_regs_t r = {.cs = 8};
    reset();
    irq_table[8].controller = get_pic_controller();
    irq_table[8].handler = handler;
    arch_irq_dispatch(&r, 0x28);
    assert_eq(4, trace_n);
    assert_eq(1, trace[0]);
    assert_eq(2, trace[1]);
    assert_eq(0xa0, trace[2]);
    assert_eq(0x20, trace[3]);
    reset();
    irq_table[0].controller = get_pic_controller();
    arch_irq_dispatch(&r, 0x20);
    assert_eq(2, trace_n);
    assert_eq(0x20, trace[1]);
    reset();
    irq_table[0].handler = handler;
    arch_irq_dispatch(&r, 0x20);
    assert_eq(2, trace_n);
    assert_eq(2, trace[1]);
    reset();
    arch_irq_dispatch(&r, 0x20);
    assert_eq(1, trace_n);
    assert_eq(0, eois);
}
TEST_FUNC(local_handler_eoi_ownership) {
    pt_regs_t r = {.cs = 8};
    reset();
    x86_irq_entry(&r, 0x38);
    assert_eq(1, ticks);
    assert_eq(0, accounts);
    assert_eq(1, eois);
    assert_eq(3, trace[0]);
    assert_eq(9, trace[1]);
    reset();
    irq_test_cpu(1);
    x86_irq_entry(&r, 0x38);
    assert_eq(0, ticks);
    assert_eq(1, accounts);
    assert_eq(1, softirqs);
    assert_eq(1, irq_test_need());
    assert_eq(1, irq_test_watchdog());
    assert_eq(1, eois);
    assert_eq(9, trace[2]);
    reset();
    ipi_init();
    generic_intr_dispatch(&r, 0x40);
    assert_eq(1, flushes);
    assert_eq(101, irq_test_gen());
    assert_eq(101, gen_at_eoi);
    assert_eq(1, eois);
    assert_eq(6, trace[0]);
    assert_eq(9, trace[1]);
    reset();
    ipi_init();
    generic_intr_dispatch(&r, 0x41);
    assert_eq(1, irq_test_need());
    assert_eq(1, need_at_eoi);
    assert_eq(100, irq_test_gen());
    assert_eq(0, flushes);
    assert_eq(1, eois);
    reset();
    generic_intr_dispatch(&r, 0x40);
    generic_intr_dispatch(&r, 0x41);
    assert_eq(0, eois);
    assert_eq(0, trace_n);
}
static void expect_bad(struct intr_event *e, pt_regs_t *r) {
    bad_calls = 0;
    if (!setjmp(bad_jb)) {
        arch_intr_handle_irq(e, r);
        assert_true(false);
    }
    assert_eq(1, bad_calls);
    assert_eq(0, trace_n);
}
TEST_FUNC(backend_revalidates_every_field_and_vector) {
    pt_regs_t r = {.cs = 8};
    struct intr_event good, e;
    reset();
    x86_intr_decode_irq(&good, &r, 0xff);
#define BAD(field, value)                                                                \
    do {                                                                                 \
        e = good;                                                                        \
        e.field = value;                                                                 \
        expect_bad(&e, &r);                                                              \
    } while (0)
    BAD(kind, INTR_SYNC);
    BAD(origin, INTR_ORIGIN_USER);
    BAD(reason, INTR_REASON_MEMORY);
    BAD(access, INTR_ACCESS_READ);
    BAD(fault_addr_valid, true);
    BAD(fault_addr, 1);
    BAD(native_status, 1);
    BAD(native_address, 1);
#undef BAD
    static const uint64_t bad[] = {0,    14,   0x1f, 0x39,  0x3f,      0x42,
                                   0x7f, 0x80, 0xfe, 0x100, UINT64_MAX};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        x86_intr_decode_irq(&e, &r, bad[i]);
        expect_bad(&e, &r);
    }
    for (unsigned c = 1; c <= 2; c++) {
        r.cs = c;
        x86_intr_decode_irq(&e, &r, 0xff);
        assert_eq(INTR_ORIGIN_UNKNOWN, e.origin);
        expect_bad(&e, &r);
    }
    expect_bad(0, &r);
    expect_bad(&good, 0);
}
TEST_FUNC(neutral_lookup_keeps_bounds_and_registration_semantics) {
    pt_regs_t r = {.cs = 8};
    reset();
    intr_handler_table[0x7e] = handler;
    intr_handler_param[0x7e] = (void *)0x1234;
    intr_dispatch_registered_vector(&r, 0x7e);
    assert_eq(0x7e, seen_vector);
    assert_eq(0x1234, seen_param);
    assert_true(seen_regs == &r);
    assert_eq(1, trace_n);
    assert_eq(0, eois);
    intr_dispatch_registered_vector(&r, 256);
    intr_dispatch_registered_vector(&r, UINT64_MAX);
    assert_eq(1, trace_n);
    assert_eq(0, bad_calls);
}
TEST_FUNC(adapter_wrong_family_and_null_rejected) {
    void (*adapters[])(pt_regs_t *, uint64_t) = {arch_irq_dispatch, generic_intr_dispatch,
                                                 x86_irq_entry};
    static const uint64_t v[] = {0x20, 0x38, 0x40, 0x41, 0xff, 0x80, 0x100, UINT64_MAX};
    pt_regs_t r = {.cs = 8};
    for (unsigned a = 0; a < 3; a++) {
        reset();
        if (!setjmp(bad_jb)) {
            adapters[a](0, a == 0 ? 0x20 : a == 1 ? 0x40 : 0x38);
            assert_true(false);
        }
        assert_eq(1, bad_calls);
        for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
            bool valid = a == 0   ? v[i] == 0x20
                         : a == 1 ? (v[i] == 0x40 || v[i] == 0x41)
                                  : (v[i] == 0x38 || v[i] == 0xff);
            if (valid)
                continue;
            reset();
            if (!setjmp(bad_jb)) {
                adapters[a](&r, v[i]);
                assert_true(false);
            }
            assert_eq(1, bad_calls);
            assert_eq(0, trace_n);
        }
    }
}
TEST_LIST_BEGIN
TEST_ENTRY(all_28_irq_vectors_both_origins), TEST_ENTRY(device_controller_ack_ownership),
    TEST_ENTRY(local_handler_eoi_ownership),
    TEST_ENTRY(backend_revalidates_every_field_and_vector),
    TEST_ENTRY(adapter_wrong_family_and_null_rejected),
    TEST_ENTRY(neutral_lookup_keeps_bounds_and_registration_semantics),
    TEST_LIST_END int main(void) {
    RUN_ALL_TESTS();
    return __test_stats.failed ? 1 : 0;
}
