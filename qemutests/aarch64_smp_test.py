#!/usr/bin/env python3
"""Cross-build the real ARM kernel and check its pre-MMU SMP ELF contract.

These checks catch slot ABI drift, inaccessible AP code/stacks and overlap
with the UEFI handoff. Hardware entry execution is covered by the QEMU suite.
"""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HANDOFF = 0x401E0000


def check_benchmark(tmp):
    # Execute the actual benchmark C with test-local hardware substitutes.
    # This catches blocking total reads after missing-done timeout, early
    # go publication, wrong iteration totals and fixed-frequency deadlines.
    # ARM instructions/order are checked by cross-build and QEMU separately.
    source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define _ARCH_CPU_H
#define _ARCH_AARCH64_SPINLOCK_H
#define _ARCH_AARCH64_BOOT_PERCPU_H
#define _ARCH_AARCH64_GIC_H
#include <arch/aarch64/boot_offsets.h>
#define NR_CPUS 8
typedef struct { unsigned long lock; } spinlock_T;
static spinlock_T bench_lock;
static uint32_t benchmark_total, done[8], online[8], commands[8];
static uint32_t active_count, missing_done, lost_increment, pauses, final_locks;
static uint64_t hz, base, elapsed, counter_reads, bsp_cost, log_cost, recheck_cost, final_lock_at;
static char output[4096];
static void spin_init(spinlock_T *lock) { lock->lock = 1; }
static void spin_lock(spinlock_T *lock)
{
    if (lock == &bench_lock && done[0]) {
        /* Even a complete done set cannot permit a final acquisition
         * after a pause or serial logging has consumed the deadline. */
        assert(elapsed < hz * 30);
        ++final_locks;
        final_lock_at = elapsed;
    }
    assert(lock->lock); lock->lock = 0;
}
static void spin_unlock(spinlock_T *lock) { assert(!lock->lock); lock->lock = 1; }
static int spin_trylock(spinlock_T *lock)
{ if (!lock->lock) return 0; lock->lock = 0; return 1; }
static uint64_t spin_lock_irqsave(spinlock_T *lock) { spin_lock(lock); return 0; }
static void spin_unlock_irqrestore(spinlock_T *lock, uint64_t flags)
{ (void)flags; spin_unlock(lock); }
static uint64_t cntfrq_el0(void) { return hz; }
static uint64_t arch_cycle_counter(void)
{
    if (++counter_reads == 3) elapsed += recheck_cost;
    return base + elapsed;
}
static void arch_cpu_pause(void) { ++pauses; assert(++elapsed <= hz * 30); }
uint32_t boot_online_get(uint32_t id) { return online[id]; }
uint32_t boot_go_get(uint32_t id) { return commands[id]; }
static uint32_t bench_done_get(uint32_t id) { return done[id]; }
static void bench_done_set(uint32_t id, uint32_t value)
{
    assert(bench_lock.lock); /* publish done only after the final unlock */
    if (value && id == lost_increment) --benchmark_total;
    if (id != missing_done) done[id] = value;
    if (id == 0) elapsed += bsp_cost;
}
void kputs(const char *s)
{
    elapsed += log_cost;
    assert(strlen(output) + strlen(s) < sizeof(output)); strcat(output, s);
}
void kputu(uint64_t value) { char s[32]; snprintf(s, sizeof(s), "%llu", (unsigned long long)value); kputs(s); }
void kputx(uint64_t value) { (void)value; }
void smp_bench_iter(uint32_t, uint32_t);
static void boot_go_set(uint32_t id, uint32_t command)
{
    assert(counter_reads); /* deadline starts before the first release */
    assert(id && id < active_count && command == 1 && !commands[id]);
    for (uint32_t i = 0; i < active_count; ++i) assert(online[i] == 1);
    commands[id] = command;
    smp_bench_iter(id, 1000000);
}
#include "kernel/arch/aarch64/test_spinlock.c"
static void reset(uint32_t active)
{
    memset(done, 0, sizeof(done)); memset(commands, 0, sizeof(commands));
    memset(online, 0, sizeof(online)); output[0] = 0;
    active_count = active; missing_done = lost_increment = 8;
    for (uint32_t id = 0; id < active; ++id) online[id] = 1;
    hz = 60; base = elapsed = counter_reads = benchmark_total = bsp_cost = 0;
    log_cost = recheck_cost = final_lock_at = pauses = final_locks = 0;
    spin_init(&bench_lock);
}
int main(void)
{
    reset(2); missing_done = 1;
    assert(!test_spinlock_smp(2));
    assert(elapsed == 1800 && strstr(output, "status=FAIL"));
    assert(strstr(output, "total=unavailable") && !strstr(output, "status=PASS"));
    puts("PASS: missing done reaches frequency deadline without lock read");
    reset(1); assert(test_spinlock_smp(1)); assert(benchmark_total == 1000000);
    assert(strstr(output, "active=1 iterations=1000000 total=1000000 status=PASS"));
    reset(2); assert(test_spinlock_smp(2)); assert(benchmark_total == 2000000);
    reset(4); assert(test_spinlock_smp(4)); assert(benchmark_total == 4000000);
    puts("PASS: exact one/two/four CPU totals from production iteration function");
    reset(2); lost_increment = 1; assert(!test_spinlock_smp(2));
    assert(benchmark_total == 1999999 && strstr(output, "status=FAIL"));
    reset(2); online[1] = 0; assert(!test_spinlock_smp(2));
    assert(!commands[1] && !benchmark_total);
    reset(2); done[1] = 1; assert(!test_spinlock_smp(2));
    assert(!commands[1] && !benchmark_total);
    reset(2); commands[1] = 2; assert(!test_spinlock_smp(2));
    assert(commands[1] == 2 && !benchmark_total);
    puts("PASS: lost increment, incomplete ACK and stale lifecycle fail");
    reset(2); hz = 1; base = UINT64_MAX - 10; missing_done = 1;
    assert(!test_spinlock_smp(2) && elapsed == 30);
    reset(2); hz = 123; missing_done = 1;
    assert(!test_spinlock_smp(2) && elapsed == 3690);
    reset(2); hz = 1; bsp_cost = 30;
    assert(!test_spinlock_smp(2) && strstr(output, "status=FAIL"));
    reset(2); hz = 1; bsp_cost = 29;
    assert(test_spinlock_smp(2) && !pauses && final_locks == 1 && final_lock_at == 29);
    reset(2); hz = 1; bsp_cost = 29; log_cost = 1;
    assert(test_spinlock_smp(2) && !pauses && final_locks == 1 && final_lock_at == 29);
    assert(elapsed > 30 && strstr(output, "total=2000000 status=PASS"));
    reset(2); hz = 1; bsp_cost = 29; recheck_cost = 1;
    assert(!test_spinlock_smp(2) && !pauses && !final_locks);
    assert(strstr(output, "total=unavailable") && strstr(output, "status=FAIL"));
    puts("PASS: completed done skips pause, snapshots before logging and rechecks final deadline");
    reset(2); hz = 0; assert(!test_spinlock_smp(2)); assert(!commands[1]);
    reset(2); hz = UINT64_MAX/30 + 1; assert(!test_spinlock_smp(2));
    assert(!commands[1]);
    puts("PASS: frequency-derived deadline, counter wrap and invalid frequency");
}
'''
    runner = tmp / "benchmark_runner.c"
    runner.write_text(source)
    executable = tmp / "benchmark_runner"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                    "-Werror", "-I.", "-Ikernel/include", str(runner),
                    "-o", str(executable)], cwd=ROOT, check=True)
    subprocess.run([str(executable)], check=True)


def check_build_controls():
    # Run make's real configuration parser and recursive build recipes.
    # Missing validation/whitelist/explicit submake args must fail here.
    config = "include kernel/arch/aarch64/make.config\nall:\n\t@echo $(ARCH_CFLAGS)\n"
    for value in ("0", "1", "7", "8", "-1", "1 2", "", "01", "x", "%"):
        result = subprocess.run(["make", "--no-print-directory", "-f", "-",
                                 "OS01_PROFILE_FILE=test", "AARCH64_SMP_TEST_NO_ACK_CPU=" + value],
                                input=config, cwd=ROOT, text=True, capture_output=True)
        if value in ("0", "1", "7"):
            assert result.returncode == 0, result.stderr
            assert "-DAARCH64_SMP_TEST_NO_ACK_CPU=" + value in result.stdout
        else:
            assert result.returncode != 0, "accepted invalid injection: " + repr(value)
    result = subprocess.run(["make", "-Bn", "PROFILE=aarch64-clang",
                             "AARCH64_SMP_TEST_NO_ACK_CPU=1", "aarch64-uefi-kernel"],
                            cwd=ROOT, text=True, capture_output=True)
    assert result.returncode == 0, result.stderr
    assert "-DAARCH64_SMP_TEST_NO_ACK_CPU=1" in result.stdout, "lost injection at recursive boundary"
    for profile, value, valid in (("aarch64-clang", "1", True),
                                  ("aarch64-clang", "0", False),
                                  ("aarch64-clang", "1 2", False),
                                  ("x86_64-clang", "1", False)):
        result = subprocess.run(["make", "-n", "PROFILE=" + profile,
                                 "AARCH64_SMP_TEST_NO_ACK_CPU=" + value,
                                 "test-aarch64-uefi-smp-no-ack"],
                                cwd=ROOT, text=True, capture_output=True)
        assert (result.returncode == 0) == valid, result.stderr
        if valid:
            assert "--cpus 2 --repeat 1" in result.stdout
            assert "--expect-no-ack 1" in result.stdout
            assert "-target aarch64" not in result.stdout, "negative target rebuilt its image"
    print("PASS: injection validation, recursive compiler define and prebuilt-only target gate")


def check_boot_core(tmp):
    # Regressions caught: optimistic CPU_ON counting, retrying firmware,
    # accepting boundary/late ACKs, truncating Aff3, absolute deadlines,
    # unvalidated counter frequency, or releasing a partial benchmark.
    source = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <arch/aarch64/smp_boot_core.h>

struct event { uint64_t at; uint32_t cpu; };
struct fake {
    uint64_t base, elapsed, call_cost;
    int32_t rc[8];
    uint32_t ack[8], go[8], calls[8], commands[8];
    uint64_t target[8], entry[8];
    struct event events[8];
    uint32_t events_len;
};
static void deliver(struct fake *f)
{
    for (uint32_t e = 0; e < f->events_len; ++e)
        if (f->elapsed >= f->events[e].at)
            f->ack[f->events[e].cpu] = 1;
}
static uint64_t counter(void *ctx)
{
    struct fake *f = ctx;
    deliver(f);
    return f->base + f->elapsed;
}
static int32_t cpu_on(void *ctx, uint64_t target, uint64_t entry, uint64_t id)
{
    struct fake *f = ctx;
    assert(id > 0 && id < 8); /* the BSP must never be started */
    assert(++f->calls[id] == 1);
    assert(f->ack[id] == 0 && f->go[id] == 0);
    f->target[id] = target;
    f->entry[id] = entry;
    f->elapsed += f->call_cost;
    return f->rc[id];
}
static uint32_t online(void *ctx, uint32_t id)
{
    return ((struct fake *)ctx)->ack[id];
}
static void command(void *ctx, uint32_t id, uint32_t value)
{
    struct fake *f = ctx;
    assert(id > 0 && id < 8);
    assert(value == 2); /* never release the benchmark from the boot core */
    f->go[id] = value;
    ++f->commands[id];
}
static void relax(void *ctx)
{
    struct fake *f = ctx;
    assert(++f->elapsed < 2000); /* catch an unbounded wait without wall time */
}
static struct aarch64_topology topology(uint32_t count)
{
    struct aarch64_topology t = {.cpu_count=count};
    t.mpidr[0] = 0x100;
    t.mpidr[1] = UINT64_C(0x0000000100000000); /* Aff3 survives CPU_ON */
    for (uint32_t i = 2; i < count; ++i) t.mpidr[i] = i;
    return t;
}
static struct smp_boot_ops ops(struct fake *f)
{
    return (struct smp_boot_ops){f, counter, cpu_on, online, command, relax};
}
static int run(struct fake *f, uint32_t count, uint64_t hz,
               struct smp_boot_result *r)
{
    struct aarch64_topology t = topology(count);
    struct smp_boot_ops o = ops(f);
    return smp_boot_run(&t, 0x40080c00, hz, &o, r);
}
static void commands_are(struct fake *f, uint32_t count, uint32_t expected)
{
    assert(f->calls[0] == 0 && f->commands[0] == 0);
    for (uint32_t i = 1; i < count; ++i) {
        assert(f->calls[i] == 1);
        assert(f->go[i] == expected);
        assert(f->commands[i] == (expected == 2 ? 1U : 0U));
    }
}
int main(void)
{
    struct fake f = {0};
    struct smp_boot_result r;
    f.events[0] = (struct event){3, 1};
    f.events[1] = (struct event){8, 2};
    f.events_len = 2;
    assert(run(&f, 3, 100, &r) == 0);
    assert(r.requested == 3 && r.online == 3 && r.online_mask == 7);
    assert(r.failure[1] == SMP_FAILURE_NONE && r.failure[2] == SMP_FAILURE_NONE);
    assert(f.target[1] == UINT64_C(0x0000000100000000));
    assert(f.entry[1] == 0x40080c00);
    commands_are(&f, 3, 0);
    puts("PASS: success, full Aff3 target and no premature go");

    const int32_t errors[] = {-1, -2, -3, -6, -7, -8, -9, 1};
    for (uint32_t e = 0; e < sizeof(errors)/sizeof(errors[0]); ++e) {
        memset(&f, 0, sizeof(f));
        f.rc[1] = errors[e];
        f.events[0] = (struct event){3, 2}; f.events_len = 1;
        assert(run(&f, 3, 100, &r) == 1);
        assert(r.online == 2 && r.online_mask == 5);
        assert(r.psci_rc[1] == errors[e] && r.failure[1] == SMP_FAILURE_CPU_ON);
        assert(r.failure[2] == SMP_FAILURE_NONE);
        commands_are(&f, 3, 2);
    }
    puts("PASS: raw CPU_ON errors, continued startup and all-AP idle");

    memset(&f, 0, sizeof(f)); f.rc[1] = -4;
    assert(run(&f, 2, 100, &r) == 1);
    assert(r.online == 1 && r.online_mask == 1 && r.psci_rc[1] == -4);
    assert(r.failure[1] == SMP_FAILURE_TIMEOUT && f.elapsed == 200);
    commands_are(&f, 2, 2);
    puts("PASS: ALREADY_ON needs a kernel ACK");

    const int32_t waiting[] = {0, -4, -5};
    for (uint32_t i = 0; i < 3; ++i) {
        memset(&f, 0, sizeof(f)); f.rc[1] = waiting[i];
        f.events[0] = (struct event){199, 1}; f.events_len = 1;
        assert(run(&f, 2, 100, &r) == 0);
        assert(r.online_mask == 3 && r.psci_rc[1] == waiting[i]);
        commands_are(&f, 2, 0);
    }
    puts("PASS: success/already-on/on-pending delayed ACK");

    memset(&f, 0, sizeof(f));
    f.events[0] = (struct event){200, 1}; f.events_len = 1;
    assert(run(&f, 2, 100, &r) == 1);
    assert(r.online_mask == 1 && r.failure[1] == SMP_FAILURE_TIMEOUT);
    commands_are(&f, 2, 2);
    puts("PASS: exact timeout boundary rejects ACK");

    memset(&f, 0, sizeof(f));
    f.events[0] = (struct event){201, 1};
    f.events[1] = (struct event){210, 2}; f.events_len = 2;
    assert(run(&f, 3, 100, &r) == 1);
    assert(f.ack[1] == 1 && r.online == 2 && r.online_mask == 5);
    commands_are(&f, 3, 2);
    puts("PASS: late ACK excluded from frozen online mask");

    memset(&f, 0, sizeof(f)); f.call_cost = 200;
    f.events[0] = (struct event){199, 1}; f.events_len = 1;
    assert(run(&f, 2, 100, &r) == 1);
    assert(r.online_mask == 1 && r.failure[1] == SMP_FAILURE_TIMEOUT);
    puts("PASS: deadline begins before CPU_ON transport");

    memset(&f, 0, sizeof(f)); f.base = UINT64_MAX - 100;
    f.events[0] = (struct event){110, 1}; f.events_len = 1;
    assert(run(&f, 2, 60, &r) == 0 && r.online_mask == 3);
    memset(&f, 0, sizeof(f)); f.base = UINT64_MAX - 100;
    assert(run(&f, 2, 60, &r) == 1 && r.online_mask == 1 && f.elapsed == 120);
    puts("PASS: counter wrap, successful ACK and timeout");

    memset(&f, 0, sizeof(f));
    assert(run(&f, 1, 100, &r) == 0);
    assert(r.online == 1 && r.online_mask == 1 && r.requested == 1);
    assert(f.calls[0] == 0 && f.calls[1] == 0 && f.elapsed == 0);
    puts("PASS: single CPU does not invoke firmware");

    memset(&f, 0, sizeof(f));
    assert(run(&f, 2, 0, &r) == -1);
    assert(run(&f, 2, UINT64_MAX/2 + 1, &r) == -1);
    assert(run(&f, 1, UINT64_MAX/2, &r) == 0);
    assert(run(&f, 0, 100, &r) == -1);
    assert(f.calls[1] == 0 && f.commands[1] == 0);
    struct aarch64_topology t = topology(2);
    struct smp_boot_ops o = ops(&f);
    assert(smp_boot_run(NULL, 4, 100, &o, &r) == -1);
    assert(smp_boot_run(&t, 4, 100, NULL, &r) == -1);
    assert(smp_boot_run(&t, 4, 100, &o, NULL) == -1);
    assert(smp_boot_run(&t, 0, 100, &o, &r) == -1);
    t.cpu_count = 9;
    assert(smp_boot_run(&t, 4, 100, &o, &r) == -1);
    t.cpu_count = 2;
    o.counter = NULL;
    assert(smp_boot_run(&t, 4, 100, &o, &r) == -1);
    o = ops(&f); o.cpu_on = NULL;
    assert(smp_boot_run(&t, 4, 100, &o, &r) == -1);
    o = ops(&f); o.online_acquire = NULL;
    assert(smp_boot_run(&t, 4, 100, &o, &r) == -1);
    o = ops(&f); o.command_release = NULL;
    assert(smp_boot_run(&t, 4, 100, &o, &r) == -1);
    o = ops(&f); o.relax = NULL;
    assert(smp_boot_run(&t, 4, 100, &o, &r) == -1);
    f.ack[1] = 1;
    assert(run(&f, 2, 100, &r) == -1);
    assert(f.calls[1] == 0 && f.commands[1] == 0);
    f.ack[1] = 0; f.ack[2] = 1;
    assert(run(&f, 3, 100, &r) == -1);
    assert(f.calls[1] == 0 && f.calls[2] == 0);
    puts("PASS: invalid frequency/input/initial ACK rejected before CPU_ON");
    return 0;
}
'''
    runner = tmp / "boot_runner.c"
    runner.write_text(source)
    executable = tmp / "boot_runner"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                    "-Werror", "-Ikernel/include",
                    "kernel/arch/aarch64/smp_boot_core.c", str(runner),
                    "-o", str(executable)], cwd=ROOT, check=True)
    subprocess.run([str(executable)], check=True)


def elf_layout(path):
    data = path.read_bytes()
    assert data[:6] == b"\x7fELF\x02\x01", "expected ELF64 little endian"
    header = struct.unpack_from("<16sHHIQQQIHHHHHH", data)
    assert header[2] == 183, "expected AArch64 ELF"
    loads = []
    for i in range(header[10]):
        ph = struct.unpack_from("<IIQQQQQQ", data, header[5] + i * header[9])
        if ph[0] == 1:
            loads.append(dict(flags=ph[1], va=ph[3], pa=ph[4],
                              filesz=ph[5], memsz=ph[6]))
    sections = [struct.unpack_from("<IIQQQQIIQQ", data,
                header[6] + i * header[11]) for i in range(header[12])]
    symbols = {}
    for sec in sections:
        if sec[1] != 2:  # SHT_SYMTAB
            continue
        strings = sections[sec[6]]
        names = data[strings[4]:strings[4] + strings[5]]
        for off in range(sec[4], sec[4] + sec[5], sec[9]):
            name, _, _, _, value, size = struct.unpack_from("<IBBHQQ", data, off)
            end = names.index(0, name)
            symbols[names[name:end].decode()] = (value, size)
    return loads, symbols


def check_elf(path):
    loads, symbols = elf_layout(path)
    assert loads, "kernel has no PT_LOAD"
    for seg in loads:
        assert 0x40080000 <= seg["pa"] < HANDOFF, "PT_LOAD outside boot RAM"
        assert seg["pa"] + seg["memsz"] <= HANDOFF, "PT_LOAD overlaps handoff"
        if seg["va"] >= 0xffff000000000000:
            assert seg["va"] - 0xffff000000000000 == seg["pa"], \
                "high-half runtime address differs from loader allocation/zeroing"

    def mapped(start, size, flags):
        return any(seg["va"] == seg["pa"] and seg["flags"] & flags == flags
                   and seg["pa"] <= start
                   and start + size <= seg["pa"] + seg["memsz"] for seg in loads)

    entry, entry_size = symbols["secondary_start"]
    assert entry_size > 0 and mapped(entry, entry_size, 5), \
        "AP entry is not executable at its physical address"
    for name in ("boot_vectors", "exception_vectors"):
        assert symbols[name][0] % 0x800 == 0, name + " is not 2 KiB aligned"
    stacks, stack_bytes = symbols["aarch64_boot_stacks"]
    assert stack_bytes == 8 * 4096, "independent boot stack allocation is incomplete"
    slots, slot_bytes = symbols["aarch64_boot_percpu"]
    assert slot_bytes == 8 * 48, "per-CPU slot ABI changed"
    tables = symbols["boot_page_tables"][0]
    tables_end = symbols["boot_page_tables_end"][0]
    assert tables % 4096 == 0 and tables_end - tables == 6 * 4096
    mpidrs, mpidr_bytes = symbols["aarch64_dtb_mpidr_table"]
    count, count_bytes = symbols["aarch64_dtb_cpu_count"]
    assert mpidr_bytes == 8 * 8 and count_bytes == 4, "topology storage ABI changed"
    ranges = [(slots, slots + slot_bytes), (tables, tables_end),
              (mpidrs, mpidrs + mpidr_bytes), (count, count + count_bytes)]
    assert all(mapped(lo, hi - lo, 6) for lo, hi in ranges), \
        "boot metadata/page tables are not identity-mapped writable RAM"
    for cpu in range(8):
        start, end = stacks + cpu * 4096, stacks + (cpu + 1) * 4096
        assert start % 16 == end % 16 == 0, "misaligned boot stack"
        assert mapped(start, end - start, 6), "stack not identity-mapped writable RAM"
        assert all(end <= lo or start >= hi for lo, hi in ranges), "boot stack overlap"
        ranges.append((start, end))
    assert symbols["_kernel_lma_end"][0] <= HANDOFF


def check_layout_compile(tmp, clang):
    # Literal expectations describe the assembly/C boundary independently of
    # the shared constants; moving online/go silently breaks the AP ACK ABI.
    source = r'''
#include "aarch64_percpu.h"
#include <arch/aarch64/boot_offsets.h>
_Static_assert(sizeof(aarch64_boot_percpu_t) == 48, "slot size");
_Static_assert(__builtin_offsetof(aarch64_boot_percpu_t, online) == 32, "ACK offset");
_Static_assert(__builtin_offsetof(aarch64_boot_percpu_t, go) == 36, "command offset");
_Static_assert(AARCH64_BOOT_PERCPU_SIZE == 48, "assembly stride");
_Static_assert(AARCH64_BOOT_ONLINE_OFFSET == 32, "assembly ACK offset");
_Static_assert(AARCH64_BOOT_GO_OFFSET == 36, "assembly command offset");
'''
    subprocess.run([clang, "--target=aarch64-none-elf", "-ffreestanding",
                    "-Wall", "-Wextra", "-Werror", "-Ikernel/include",
                    "-Ikernel/arch/aarch64", "-x", "c", "-c", "-",
                    "-o", str(tmp / "layout.o")], input=source, text=True,
                   cwd=ROOT, check=True)


def check_linker_guard(tmp, clang):
    source = r'''
.section .boot.text.first,"ax"
.global _start
_start: b _start
.section .bss,"aw",@nobits
.space 0x170000
'''
    obj = tmp / "oversized.o"
    subprocess.run([clang, "--target=aarch64-none-elf", "-x", "assembler",
                    "-c", "-", "-o", str(obj)], input=source, text=True, check=True)
    result = subprocess.run([os.environ.get("LD_LLD", "ld.lld"), "-T",
                             str(ROOT / "kernel/arch/aarch64/linker.ld"),
                             str(obj), "-o", str(tmp / "oversized.elf")],
                            text=True, capture_output=True)
    assert result.returncode != 0, "linker accepted a kernel overlapping UEFI handoff"
    assert "kernel overlaps UEFI handoff" in result.stderr, result.stderr


def main():
    clang = os.environ.get("CLANG", "clang")
    with tempfile.TemporaryDirectory(prefix="aarch64-smp-") as tmp:
        tmp = Path(tmp)
        check_boot_core(tmp)
        check_benchmark(tmp)
        check_build_controls()
        if "--host-only" in sys.argv:
            return
        check_layout_compile(tmp, clang)
        check_linker_guard(tmp, clang)
    subprocess.run(["make", "PROFILE=aarch64-clang", "aarch64-uefi-kernel"],
                   cwd=ROOT, check=True)
    check_elf(ROOT / "build/aarch64-clang/kernel/kernel.elf")
    print("aarch64_smp: slot ABI, linker handoff guard and kernel ELF checks passed")


if __name__ == "__main__":
    main()
