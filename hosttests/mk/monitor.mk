# Monitor subsystem host tests
$(eval $(call register_host_test,test_monitor_accounting,monitor,protocol-or-legacy))
$(eval $(call register_host_test,test_monitor_collect,monitor,protocol-or-legacy))
$(eval $(call register_host_test,test_monitor_file,monitor,protocol-or-legacy))
$(eval $(call register_host_test,test_monitor_model,monitor,protocol-or-legacy))
$(eval $(call register_host_test,test_monitor_pmm,monitor,protocol-or-legacy))
$(eval $(call register_host_test,test_monitor_render,monitor,protocol-or-legacy))
$(eval $(call register_host_test,test_thermal_affinity,monitor,protocol-or-legacy))
$(eval $(call register_host_test,test_thermal_decode,monitor,protocol-or-legacy))
$(eval $(call register_host_test,test_thermal_fixup,monitor,protocol-or-legacy))

TEST_BINS += \
    $(TEST_BLD)/test_monitor_file.elf \
    $(TEST_BLD)/test_monitor_accounting.elf \
    $(TEST_BLD)/test_monitor_pmm.elf \
    $(TEST_BLD)/test_monitor_collect.elf \
    $(TEST_BLD)/test_monitor_model.elf \
    $(TEST_BLD)/test_monitor_render.elf \
    $(TEST_BLD)/test_thermal_fixup.elf \
    $(TEST_BLD)/test_thermal_affinity.elf \
    $(TEST_BLD)/test_thermal_decode.elf

# Monitor plan Task 1: VFS per-file snapshot lifecycle hooks.  Reuses
# the gfx harness's production objects (kernel/fs/file.c + devfs.c via
# GFX_HOST_CFLAGS / gfx_test_runtime.h) so the fixture drives the REAL
# file_alloc / file_get / file_put / file_free / fd_read paths and the
# real devfs register/open/release protocol, not mocks of them.  The
# snapshot filesystem callbacks live in the test TU (the procfs
# counterpart is monitor plan Task 4).  Tested production symbols:
# vfs_file_open, vfs_file_read, vfs_file_release, fd_read,
# devfs_register_chrdev, devfs_open_node, devfs_release_file.
.PHONY: test-monitor-file
test-monitor-file: $(TEST_BLD)/test_monitor_file.elf
	$<

$(TEST_BLD)/test_monitor_file.o: $(TEST_CASES)/test_monitor_file.c \
		$(TEST_MOCK)/gfx_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_monitor_file.elf: $(TEST_BLD)/test_monitor_file.o \
		$(TEST_BLD)/gfx_devfs_production.o \
		$(TEST_BLD)/gfx_file_production.o \
		$(TEST_BLD)/serial_sink_stub.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

# Monitor plan Task 2: per-CPU tick accounting.  Host-compiles the REAL
# kernel/sched/accounting.c against mock/accounting_test_runtime.h,
# which short-circuits sched/task.h + percpu/percpu.h and provides a
# test-controlled cpu_id() / percpu_data[] / current, so the fixture
# drives the production accounting code (not a mock of it).  The test TU
# reuses test_platform.h's percpu_t / spinlock stubs (FRAMEWORK_INC
# preincludes it FIRST, so the mock header takes its test-TU branch);
# the production object must NOT see FRAMEWORK_INC — test_platform.h
# typedefs a stale percpu_t and a constant cpu_id().
ACC_HOST_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC) $(LIBC_INC)
ACC_PROD_CFLAGS := $(ACC_HOST_CFLAGS) \
                   -include $(TEST_MOCK)/accounting_test_runtime.h

$(TEST_BLD)/accounting_production.o: $(TESTS_DIR)/kernel/sched/accounting.c \
		$(TEST_MOCK)/accounting_test_runtime.h \
		$(TESTS_DIR)/kernel/include/sched/accounting.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(ACC_PROD_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_monitor_accounting.o: $(TEST_CASES)/test_monitor_accounting.c \
		$(TEST_MOCK)/accounting_test_runtime.h \
		$(TESTS_DIR)/kernel/include/sched/accounting.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FRAMEWORK_INC) $(ACC_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_monitor_accounting.elf: $(TEST_BLD)/test_monitor_accounting.o \
		$(TEST_BLD)/accounting_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test-monitor-accounting
test-monitor-accounting: $(TEST_BLD)/test_monitor_accounting.elf
	$<

# Monitor plan Task 3: locked PMM usage snapshot.  Links the REAL
# kernel/memory/pmm.c via the SAME PMM_SHADOW_INC harness as
# test_pmm_ram_rel_index (mock arch/spinlock.h + arch/cpu.h shadow the
# privileged-asm headers; everything else is the production header).
# The fixture hand-builds PMMngr (zone counters + bits_map) instead of
# driving pmm_init's boot_context path — out of scope for a snapshot
# test — and parks a static backing buffer at the Phy_To_Virt image of
# the pool page so alloc_4k_page's new-pool path writes into real host
# memory.  Asserted: the 3-pages/1-in-use fixture T=6291456 /
# U=2097152 / F=4194304, T==U+F across page alloc/free, in-pool 4KB
# alloc/free leaving U untouched, T=0 → zero stats, Σzone summing.
$(TEST_BLD)/test_monitor_pmm.o: $(TEST_CASES)/test_monitor_pmm.c \
		$(TEST_MOCK)/pmm_include/arch/spinlock.h \
		$(TESTS_DIR)/kernel/include/memory/pmm.h \
		$(TESTS_DIR)/kernel/include/memory/memory.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) -I$(TEST_INC) -c $< -o $@

$(TEST_BLD)/test_monitor_pmm.elf: $(TEST_BLD)/test_monitor_pmm.o \
		$(TEST_BLD)/pmm_production.o \
		$(TEST_BLD)/pmm_arch_default.o \
		$(TEST_BLD)/pmm_boot_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test-monitor-pmm
test-monitor-pmm: $(TEST_BLD)/test_monitor_pmm.elf
	$<

# Monitor plan Task 4: task identity + procfs snapshot producers.
# Host-compiles the REAL kernel/fs/procfs_snap.c (the lock-taking
# snapshot producers + /proc root task readdir), kernel/fs/procfs_fmt.c
# (pure formatting + Name encoding), kernel/sched/identity.c (birth_id/
# start_ns/comm lifecycle) and kernel/arch/thermal.c (weak thermal
# defaults + per-CPU cache) against mock/procfs_test_runtime.h, which
# shadows sched/task.h / percpu / slab and provides a test-controlled
# task list, monotonic clock, kmalloc fail-injection and a
# task_list_lock observation hook that can simulate the winning
# concurrent delete (unlink + poison + free inside the acquisition).
# The fixture stubs sched_accounting_read / pmm_get_stats — the real
# units keep their own Task-2/Task-3 harnesses (test-monitor-accounting,
# test-monitor-pmm).  Asserted: fork birth/runtime/comm protocol, exec
# rename rules, Name encoding (63 control bytes → 252, one line,
# backslash doubling, \xHH, exact-fit EOVERFLOW), status/stat/thermal/
# meminfo formats (Version: 1 + SampleNs everywhere, meminfo kB),
# snapshot stability across task disappearance, -ENOMEM/-EOVERFLOW
# rollback, readdir zombie visibility + concurrent-delete safety.
PROCFS_HOST_CFLAGS := $(HOST_CFLAGS) -ffunction-sections -fdata-sections \
    -Wno-unused-function -D__SPINLOCK_H__ -DOS01_HOST_TEST \
    -include $(TEST_MOCK)/procfs_test_runtime.h \
    -I$(TEST_MOCK)/include $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC)

# The pure formatter TU needs no mock surface at all.
FMT_HOST_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC) $(LIBC_INC)

$(TEST_BLD)/procfs_snap_production.o: $(TESTS_DIR)/kernel/fs/procfs_snap.c \
		$(TEST_MOCK)/procfs_test_runtime.h \
		$(TESTS_DIR)/kernel/include/fs/procfs_fmt.h \
		$(TESTS_DIR)/kernel/include/fs/procfs.h \
		$(TESTS_DIR)/kernel/include/arch/thermal.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PROCFS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/procfs_fmt_production.o: $(TESTS_DIR)/kernel/fs/procfs_fmt.c \
		$(TESTS_DIR)/kernel/include/fs/procfs_fmt.h \
		$(TESTS_DIR)/kernel/include/arch/thermal.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FMT_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/identity_production.o: $(TESTS_DIR)/kernel/sched/identity.c \
		$(TEST_MOCK)/procfs_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PROCFS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/thermal_production.o: $(TESTS_DIR)/kernel/arch/thermal.c \
		$(TESTS_DIR)/kernel/include/arch/thermal.h \
		$(TEST_MOCK)/procfs_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PROCFS_HOST_CFLAGS) -c $< -o $@

# ── monitor thermal plan, Task 1: instruction-scoped safe RDMSR ──
# Host fixture test_thermal_fixup.elf links the REAL production
# kernel/arch/x86_64/cpu/msr_safe.{S,c} — there is no host-side
# clone.  The preprocessor needs KERNEL_INC so .S can pull
# <arch/x86_64/linkage.h>; the C side needs the same plus the host
# mock surface so spinlock/clocksource/... stubs are visible.
MSR_HOST_CFLAGS := $(HOST_CFLAGS) -ffunction-sections -fdata-sections \
                   -Wno-unused-function -D__SPINLOCK_H__ -DOS01_HOST_TEST \
                   -I$(TEST_MOCK)/include $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC)

$(TEST_BLD)/msr_safe_production.o: $(TESTS_DIR)/kernel/arch/x86_64/cpu/msr_safe.S \
		$(TESTS_DIR)/kernel/include/arch/x86_64/linkage.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/msr_safe.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(MSR_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/msr_safe_fixup_production.o: $(TESTS_DIR)/kernel/arch/x86_64/cpu/msr_safe_fixup.c \
		$(TESTS_DIR)/kernel/include/arch/x86_64/msr_safe.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/regs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(MSR_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_thermal_fixup.o: $(TEST_CASES)/test_thermal_fixup.c \
		$(TESTS_DIR)/kernel/include/arch/x86_64/msr_safe.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/regs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(MSR_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_thermal_fixup.elf: $(TEST_BLD)/test_thermal_fixup.o \
		$(TEST_BLD)/msr_safe_production.o \
		$(TEST_BLD)/msr_safe_fixup_production.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

.PHONY: test-thermal-fixup
test-thermal-fixup: $(TEST_BLD)/test_thermal_fixup.elf
	$<

# ── monitor thermal plan, Task 2: BSP-pinned kernel threads ──
# Host fixture test_thermal_affinity.elf compiles against
# <sched/task.h> + <sched/internal.h> + <arch/x86_64/msr_safe.h>
# (regression on Task 1).  The runtime scheduler checks live in
# kernel/selftest/test_thermal_affinity.c (in-kernel).
AFFINITY_HOST_CFLAGS := $(HOST_CFLAGS) -ffunction-sections -fdata-sections \
                        -Wno-unused-function -D__SPINLOCK_H__ -DOS01_HOST_TEST \
                        -I$(TEST_MOCK)/include $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC)

$(TEST_BLD)/test_thermal_affinity.o: $(TEST_CASES)/test_thermal_affinity.c \
		$(TESTS_DIR)/kernel/include/sched/task.h \
		$(TESTS_DIR)/kernel/include/sched/internal.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/msr_safe.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(AFFINITY_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_thermal_affinity.elf: $(TEST_BLD)/test_thermal_affinity.o \
		$(TEST_BLD)/msr_safe_production.o \
		$(TEST_BLD)/msr_safe_fixup_production.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

.PHONY: test-thermal-affinity
test-thermal-affinity: $(TEST_BLD)/test_thermal_affinity.elf
	$<

# ── monitor thermal plan, Task 3: pure IA32_THERM_STATUS decoder ──
# Pure decode lives in kernel/arch/x86_64/cpu/thermal_decode.c so
# both the kernel build and the host fixture can link it without
# pulling in the rest of the backend (which needs kernel symbols).
THERMAL_DECODE_HOST_CFLAGS := $(HOST_CFLAGS) -ffunction-sections -fdata-sections \
                              -Wno-unused-function -D__SPINLOCK_H__ -DOS01_HOST_TEST \
                              -I$(TEST_MOCK)/include $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC)

$(TEST_BLD)/thermal_decode_production.o: \
		$(TESTS_DIR)/kernel/arch/x86_64/cpu/thermal_decode.c \
		$(TESTS_DIR)/kernel/include/arch/thermal.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(THERMAL_DECODE_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_thermal_decode.o: $(TEST_CASES)/test_thermal_decode.c \
		$(TESTS_DIR)/kernel/include/arch/thermal.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(THERMAL_DECODE_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_thermal_decode.elf: $(TEST_BLD)/test_thermal_decode.o \
		$(TEST_BLD)/thermal_decode_production.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

.PHONY: test-thermal-decode
test-thermal-decode: $(TEST_BLD)/test_thermal_decode.elf
	$<

$(TEST_BLD)/test_monitor_procfs.o: $(TEST_CASES)/test_monitor_procfs.c \
		$(TEST_MOCK)/procfs_test_runtime.h \
		$(TESTS_DIR)/kernel/include/fs/procfs_fmt.h \
		$(TESTS_DIR)/kernel/include/fs/procfs.h \
		$(TESTS_DIR)/kernel/include/sched/accounting.h \
		$(TESTS_DIR)/kernel/include/memory/pmm.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PROCFS_HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_monitor_procfs.elf: $(TEST_BLD)/test_monitor_procfs.o \
		$(TEST_BLD)/procfs_snap_production.o \
		$(TEST_BLD)/procfs_fmt_production.o \
		$(TEST_BLD)/identity_production.o \
		$(TEST_BLD)/thermal_production.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

.PHONY: test-monitor-procfs
test-monitor-procfs: $(TEST_BLD)/test_monitor_procfs.elf
	$<

# Monitor plan Task 6: user-space collection + strict parsing.  The
# fixture drives the REAL user/monitor/collect.c (freestanding userland
# code, OS01 libc headers) against a fixture-owned fake procfs.  The
# production TU's libc calls are renamed onto mon_test_* shims with -D
# (open/read/close/opendir/readdir/closedir/malloc/free), giving the
# fixture an interposition seam WITHOUT relying on host-libc symbol
# overriding: the fixture serves per-path text or -ENOENT, chunks
# reads, counts open fds (NOFILE discipline) and injects allocation
# failures.  Asserted: §6 validity rules (required/duplicate/overflow/
# illegal -> -EINVAL, unknown keys ignored, order/whitespace
# independent, Version must be 1), meminfo kB->bytes with T=0 invalid,
# Name decode (\\ and \xHH only, no decoded NUL, <=63 bytes, name is a
# copy), thermal fixed reason table + unknown marker + State-decided
# usability, PID all-decimal dedup, ENOENT skip, per-PID
# close-before-next-open with max_open_fds <= baseline+2, overlong
# files invalid, per-metric isolation, OOM round rollback (-ENOMEM).
MONITOR_COLLECT_CFLAGS := $(HOST_CFLAGS) $(LIBC_INC) -I$(TESTS_DIR)/user \
    -Dopen=mon_test_open -Dclose=mon_test_close -Dread=mon_test_read \
    -Dopendir=mon_test_opendir -Dreaddir=mon_test_readdir \
    -Dclosedir=mon_test_closedir \
    -Dmalloc=mon_test_malloc -Dfree=mon_test_free

$(TEST_BLD)/monitor_collect_production.o: $(TESTS_DIR)/user/monitor/collect.c \
		$(TESTS_DIR)/user/monitor/collect.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(MONITOR_COLLECT_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_monitor_collect.o: $(TEST_CASES)/test_monitor_collect.c \
		$(TESTS_DIR)/user/monitor/collect.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) \
	  -I$(TESTS_DIR)/user -c $< -o $@

$(TEST_BLD)/test_monitor_collect.elf: $(TEST_BLD)/test_monitor_collect.o \
		$(TEST_BLD)/monitor_collect_production.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

.PHONY: test-monitor-collect
test-monitor-collect: $(TEST_BLD)/test_monitor_collect.elf
	$<

# Monitor plan Task 7: delta model + bounded history.  The fixture
# drives the REAL user/monitor/model.c (freestanding userland code, OS01
# libc headers) with hand-built monitor_sample rounds.  The production
# TU's malloc/free are renamed onto mon_model_* shims with -D (the same
# interposition seam as monitor_collect_production.o), so the fixture
# can inject an allocation failure and prove an OOM round returns
# -ENOMEM WITHOUT destroying the previous display rows or the delta
# baseline.  Asserted (spec §2.3/§5.3): dual-core busy deltas 100/0 +
# idle 0/100 -> total 5000 / process 10000, all-busy -> 10000, one-decal
# rounding (1/3 -> 3333, 2/3 -> 6667), first round / zero denominator /
# counter regression / epoch change / differing tick_hz -> invalid with
# the new counters kept as the rebuilt baseline, new-onlined cores
# never contribute their accumulated history to the period delta,
# migration keeps runtime, exec same-birth continues the delta,
# PID-same-value new birth invalid, vanished PIDs removed (history
# with them), 60-slot ring wraparound with empty points (valid bit
# clear) for missing rounds and no stale values copied as current,
# near-UINT64_MAX counters delta without overflow (compare-then-
# subtract + 128-bit scaled intermediates, remainder via n-(n/d)*d),
# process display clamped at 10000 with the diagnostic 128-bit raw
# kept, sorting (processes CPU% desc / PID asc / invalid last; CPUs
# CPU% desc) and the init/update/destroy ownership contract.
MONITOR_MODEL_CFLAGS := $(HOST_CFLAGS) $(LIBC_INC) -I$(TESTS_DIR)/user \
    -Dmalloc=mon_model_malloc -Dfree=mon_model_free

$(TEST_BLD)/monitor_model_production.o: $(TESTS_DIR)/user/monitor/model.c \
		$(TESTS_DIR)/user/monitor/model.h \
		$(TESTS_DIR)/user/monitor/collect.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(MONITOR_MODEL_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_monitor_model.o: $(TEST_CASES)/test_monitor_model.c \
		$(TESTS_DIR)/user/monitor/model.h \
		$(TESTS_DIR)/user/monitor/collect.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) \
	  -I$(TESTS_DIR)/user -c $< -o $@

$(TEST_BLD)/test_monitor_model.elf: $(TEST_BLD)/test_monitor_model.o \
		$(TEST_BLD)/monitor_model_production.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

.PHONY: test-monitor-model
test-monitor-model: $(TEST_BLD)/test_monitor_model.elf
	$<

# Monitor plan Task 8: terminal dashboard, sampling loop and terminal
# lifecycle.  Drives the REAL user/monitor/render.c (all output through
# a renamed fprintf -> mon_test_fprintf) and user/monitor/monitor.c
# (renamed isatty/tcgetattr/tcsetattr/ioctl/poll/clock_gettime/signal/
# read + monitor_collect/monitor_sample_destroy, errno -> mon_test_errno,
# and main -> monitor_main so the fixture keeps its own entry point).
# model.c is linked link the real delta engine, so the loop's first
# round renders "--" and its second renders a true "50.0%".  Asserted
# (plan Task 8 Step 1): 5000 -> "50.0%" / invalid -> "--" / one-decimal
# rounding, the "PMM 占用" MiB label, Source-driven temperature labels,
# stale expiry markers, Name ESC re-escaping (no raw control byte ever
# reaches the terminal), narrow-terminal truncation that never splits an
# escape sequence, the single exit path (termios_restored &&
# cursor_restored) for q/SIGINT/SIGTERM/EOF/init-failure, no uninitialized
# termios write-back, a 3-second overrun producing exactly one catch-up
# sample, and finite plain --samples output with unknown-argument
# rejection.
MONITOR_RENDER_CFLAGS := $(HOST_CFLAGS) $(LIBC_INC) -I$(TESTS_DIR)/user
MONITOR_MAIN_CFLAGS := $(MONITOR_RENDER_CFLAGS) \
    -Dmain=monitor_main \
    -Disatty=mon_test_isatty \
    -Dtcgetattr=mon_test_tcgetattr \
    -Dtcsetattr=mon_test_tcsetattr \
    -Dioctl=mon_test_ioctl \
    -Dpoll=mon_test_poll \
    -Dclock_gettime=mon_test_clock_gettime \
    -Dsignal=mon_test_signal \
    -Dread=mon_test_read \
    -Dmonitor_collect=mon_test_collect \
    -Dmonitor_sample_destroy=mon_test_sample_destroy \
    -Derrno=mon_test_errno
MONITOR_RENDER_PROD_CFLAGS := $(MONITOR_RENDER_CFLAGS) -Dfprintf=mon_test_fprintf -Dfflush=mon_test_fflush

$(TEST_BLD)/monitor_render_production.o: $(TESTS_DIR)/user/monitor/render.c \
		$(TESTS_DIR)/user/monitor/render.h \
		$(TESTS_DIR)/user/monitor/model.h \
		$(TESTS_DIR)/user/monitor/collect.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(MONITOR_RENDER_PROD_CFLAGS) -c $< -o $@

$(TEST_BLD)/monitor_main_production.o: $(TESTS_DIR)/user/monitor/monitor.c \
		$(TESTS_DIR)/user/monitor/render.h \
		$(TESTS_DIR)/user/monitor/model.h \
		$(TESTS_DIR)/user/monitor/collect.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(MONITOR_MAIN_CFLAGS) -c $< -o $@

$(TEST_BLD)/monitor_render_model.o: $(TESTS_DIR)/user/monitor/model.c \
		$(TESTS_DIR)/user/monitor/model.h \
		$(TESTS_DIR)/user/monitor/collect.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(MONITOR_RENDER_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_monitor_render.o: $(TEST_CASES)/test_monitor_render.c \
		$(TESTS_DIR)/user/monitor/render.h \
		$(TESTS_DIR)/user/monitor/model.h \
		$(TESTS_DIR)/user/monitor/collect.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) \
	  -I$(TESTS_DIR)/user -c $< -o $@

$(TEST_BLD)/test_monitor_render.elf: $(TEST_BLD)/test_monitor_render.o \
		$(TEST_BLD)/monitor_render_production.o \
		$(TEST_BLD)/monitor_main_production.o \
		$(TEST_BLD)/monitor_render_model.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

.PHONY: test-monitor-render
test-monitor-render: $(TEST_BLD)/test_monitor_render.elf
	$<

# Gfx device test (Task 2): host-compiles the REAL
