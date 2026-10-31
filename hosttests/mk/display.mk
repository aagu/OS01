# Display subsystem host tests
$(eval $(call register_host_test,test_bga_non_x86,display,protocol-or-legacy))
$(eval $(call register_host_test,test_bga_resolution,display,protocol-or-legacy))
$(eval $(call register_host_test,test_display_owner,display,protocol-or-legacy))
$(eval $(call register_host_test,test_fb_resolution_hooks,display,protocol-or-legacy))
$(eval $(call register_host_test,test_fb_resolution_ioctl,display,protocol-or-legacy))
$(eval $(call register_host_test,test_fb_state,display,protocol-or-legacy))
$(eval $(call register_host_test,test_fb_uapi,display,protocol-or-legacy))
$(eval $(call register_host_test,test_fb_writers,display,protocol-or-legacy))
$(eval $(call register_host_test,test_gfx_client,display,protocol-or-legacy))
$(eval $(call register_host_test,test_gfx_device,display,protocol-or-legacy))
$(eval $(call register_host_test,test_gfx_fault_bridge,display,protocol-or-legacy))
$(eval $(call register_host_test,test_gfx_file_lifecycle,display,protocol-or-legacy))
$(eval $(call register_host_test,test_gfx_primitives,display,protocol-or-legacy))
$(eval $(call register_host_test,test_setres,display,protocol-or-legacy))

TEST_BINS += \
    $(TEST_BLD)/test_gfx_file_lifecycle.elf \
    $(TEST_BLD)/test_gfx_fault_bridge.elf \
    $(TEST_BLD)/test_gfx_device.elf \
    $(TEST_BLD)/test_gfx_client.elf \
    $(TEST_BLD)/test_gfx_primitives.elf \
    $(TEST_BLD)/test_fb_uapi.elf \
    $(TEST_BLD)/test_fb_state.elf \
    $(TEST_BLD)/test_fb_writers.elf \
    $(TEST_BLD)/test_bga_resolution.elf \
    $(TEST_BLD)/test_bga_non_x86.elf \
    $(TEST_BLD)/test_fb_resolution_ioctl.elf \
    $(TEST_BLD)/test_fb_resolution_hooks.elf \
    $(TEST_BLD)/test_setres.elf \
    $(TEST_BLD)/test_display_owner.elf

# kernel/fs/devfs.c + kernel/fs/file.c against gfx_test_runtime.h,
# which shadows the heavyweight kernel headers (sched/task.h,
# tty/tty.h, tty/pty.h, driver/serial.h, ...) so the production
# sources compile unmodified.  The test TU provides the stub
# surface the production sources reference (kmalloc/kfree, current,
# schedule, get_dev_tty, ...).  Tested production symbols:
# devfs_register_chrdev, devfs_open_node, devfs_ioctl_file,
# devfs_release_file, file_alloc, file_get, file_put, fd_ioctl.
GFX_HOST_CFLAGS := $(HOST_CFLAGS) -ffunction-sections -fdata-sections \
    -Wno-unused-function -D__SPINLOCK_H__ -DOS01_HOST_TEST \
    -include $(TEST_MOCK)/gfx_test_runtime.h \
    -I$(TEST_MOCK)/include $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC)

$(TEST_BLD)/gfx_devfs_production.o: $(TESTS_DIR)/kernel/fs/devfs.c \
		$(TEST_MOCK)/gfx_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/gfx_file_production.o: $(TESTS_DIR)/kernel/fs/file.c \
		$(TEST_MOCK)/gfx_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_gfx_file_lifecycle.o: $(TEST_CASES)/test_gfx_file_lifecycle.c \
		$(TEST_MOCK)/gfx_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_gfx_file_lifecycle.elf: $(TEST_BLD)/test_gfx_file_lifecycle.o \
		$(TEST_BLD)/gfx_gfx_production.o \
		$(TEST_BLD)/gfx_devfs_production.o \
		$(TEST_BLD)/gfx_file_production.o \
		$(TEST_BLD)/serial_sink_stub.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

test-gfx-file-lifecycle: $(TEST_BLD)/test_gfx_file_lifecycle.elf
	$<


# kernel/driver/gfx.c against gfx_test_runtime.h, which provides the
# heavy stubs the lifecycle test already declares (kmalloc/kfree,
# current, schedule, vfs_node_get/put, lwip, ...).  kernel/driver/fb.c
# is NOT compiled (VMA/VMM/scheduler headers are out of scope for
# this test); the test TU mocks fb_get_info and fb_write_row against
# a host-heap fb buffer so the gfx device's present contract is
# observed end-to-end.  Tested production symbols: gfx_ops (open,
# ioctl_file, release_file).
$(TEST_BLD)/gfx_gfx_production.o: $(TESTS_DIR)/kernel/driver/gfx.c \
		$(TESTS_DIR)/kernel/include/driver/gfx.h \
		$(TESTS_DIR)/kernel/include/uapi/gfx.h \
		$(TEST_MOCK)/gfx_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_HOST_CFLAGS) -c $< -o $@

# Partial-present Task 1 Step 2: host SIGSEGV bridge for the
# in-process inline-memcpy fault tests.  Compiled with HOST_CC /
# HOST_CFLAGS only — NO KERNEL_INC, NO LIBC_INC, NO force-include,
# NO GFX_HOST_CFLAGS, NO production-header chain.  The bridge .c is
# the ONLY TU in the binary that talks to glibc's signal ABI; the
# test TU includes <signal.h> only for the sigprocmask probe that
# confirms restore() puts the host SIGSEGV mask back to the
# pre-install value.  Linked ONLY into test_gfx_device.elf (the
# gfx_device tests are the only ones that exercise fault_jmp).
$(TEST_BLD)/gfx_fault_bridge.o: $(TEST_MOCK)/gfx_fault_bridge.c \
		$(TEST_MOCK)/gfx_fault_bridge.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

# Bridge self-test: links the bridge and the test TU only — does NOT
# pull in any production code.  Lets us verify install/restore +
# longjmp-on-fault before the new rect-present tests depend on it.
# Mirrors the GFX_CLIENT_*_CFLAGS pattern: errno must expand to glibc's
# TLS accessor (hosttests/include/errno.h declares a non-TLS reference,
# which trips the .tbss/.data mismatch at link time).
$(TEST_BLD)/test_gfx_fault_bridge.o: $(TEST_CASES)/test_gfx_fault_bridge.c \
		$(TEST_MOCK)/gfx_fault_bridge.h \
		$(TEST_INC)/test_framework.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TEST_MOCK) -I$(TEST_INC) \
		-D'errno=(*__errno_location())' \
		-include $(TEST_INC)/gfx_client_errno.h -c $< -o $@

$(TEST_BLD)/test_gfx_fault_bridge.elf: $(TEST_BLD)/test_gfx_fault_bridge.o \
		$(TEST_BLD)/gfx_fault_bridge.o
	$(HOST_CC) -o $@ $^

test-gfx-fault-bridge: $(TEST_BLD)/test_gfx_fault_bridge.elf
	$<

$(TEST_BLD)/test_gfx_device.o: $(TEST_CASES)/test_gfx_device.c \
		$(TESTS_DIR)/kernel/include/driver/gfx.h \
		$(TESTS_DIR)/kernel/include/uapi/gfx.h \
		$(TEST_MOCK)/gfx_test_runtime.h \
		$(TEST_MOCK)/gfx_fault_bridge.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TEST_MOCK) -c $< -o $@

$(TEST_BLD)/test_gfx_device.elf: $(TEST_BLD)/test_gfx_device.o \
		$(TEST_BLD)/gfx_gfx_production.o \
		$(TEST_BLD)/gfx_devfs_production.o \
		$(TEST_BLD)/gfx_file_production.o \
		$(TEST_BLD)/gfx_fault_bridge.o \
		$(TEST_BLD)/serial_sink_stub.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

test-gfx-device: $(TEST_BLD)/test_gfx_device.elf
	$<

# Gfx client lifecycle test (Task 3, 2D graphics API plan): host-compiles
# the REAL libgfx/gfx.c (open/close/get_info/set_clip/present) against the
# HOST compiler — no --target, no sysroot, no -ffreestanding — and links
# it through -Wl,--wrap=open,--wrap=close,--wrap=ioctl,--wrap=malloc,
# --wrap=calloc,--wrap=free so the test TU observes libgfx's libc call
# pattern (every reference is routed through the wrap stubs; libc.so's
# internal callers resolve intra-library and don't flow through .elf's
# wrapped symbols).
#
# IMPORTANT: this rule deliberately does NOT add $(TEST_INC) to the
# include path.  hosttests/include/errno.h declares `extern int errno;`
# (OS01's non-TLS errno), but host glibc provides a TLS errno via
# `__errno_location`; linking libgfx_gfx.o + test_gfx_client.o against
# libc.so with the non-TLS reference yields the "TLS definition in
# libc.so section .tbss mismatches non-TLS reference" link error.
# Skipping $(TEST_INC) lets the host compiler pick up glibc's <errno.h>,
# which keeps libgfx and test on the same TLS-aware errno.  The test TU
# only needs <gfx.h> (from the libgfx source dir) and <uapi/gfx.h>
# (from kernel UAPI) — both delivered below.
#
# Force-include the errno ABI shim (gfx_client_errno.h) and define
# `errno` to glibc's TLS accessor so every `errno` reference in
# libgfx/gfx.c expands to `(*__errno_location())`.  Without these,
# the compiled TU holds a non-TLS int reference that the linker
# rejects.  The shim only declares `__errno_location`; the -D
# supplies the macro that turns each `errno` reference into a call.
#
# libgfx/gfx.c compiles against this CFLAGS.  The test TU also adds
# -I$(TEST_INC) for test_framework.h resolution (its source has
# `#include "test_framework.h"`); the errno ABI shim is force-included
# AFTER that include resolves, so the OS01 errno.h macro constants
# (EPERM, ENOENT, ENODEV, ...) come from hosttests/include and the
# `errno` reference itself still expands to glibc's TLS accessor.
GFX_CLIENT_PROD_CFLAGS := $(HOST_CFLAGS) \
    -I$(TESTS_DIR)/libgfx -I$(TESTS_DIR)/kernel/include \
    -D'errno=(*__errno_location())' \
    -include $(TEST_INC)/gfx_client_errno.h

GFX_CLIENT_TEST_CFLAGS := $(HOST_CFLAGS) \
    -I$(TESTS_DIR)/libgfx -I$(TESTS_DIR)/kernel/include \
    -I$(TEST_INC) \
    -D'errno=(*__errno_location())' \
    -include $(TEST_INC)/gfx_client_errno.h

# Back-compat alias for the existing rule.
GFX_CLIENT_HOST_CFLAGS := $(GFX_CLIENT_PROD_CFLAGS)

$(TEST_BLD)/libgfx_gfx_production.o: $(TESTS_DIR)/libgfx/gfx.c \
		$(TESTS_DIR)/libgfx/gfx.h \
		$(TESTS_DIR)/libgfx/internal.h \
		$(TESTS_DIR)/kernel/include/uapi/gfx.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_CLIENT_HOST_CFLAGS) -c $< -o $@

# The test TU also does NOT see test_platform.h (the OS01 host mock
# surface) — it only uses host libc functions (printf, exit, ...).
# test_framework.h is host-safe (no OS01 headers) and resolves via
# -I$(TEST_INC) above.
$(TEST_BLD)/test_gfx_client.o: $(TEST_CASES)/test_gfx_client.c \
		$(TESTS_DIR)/libgfx/gfx.h \
		$(TESTS_DIR)/kernel/include/uapi/gfx.h \
		$(TEST_INC)/test_framework.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_CLIENT_TEST_CFLAGS) -c $< -o $@

# __real_calloc is generated by --wrap=calloc; declare it explicitly so
# the wrapper compiles without an implicit-declaration warning.
$(TEST_BLD)/test_gfx_client.elf: $(TEST_BLD)/test_gfx_client.o \
		$(TEST_BLD)/libgfx_gfx_production.o
	$(HOST_CC) -Wl,--wrap=open,--wrap=close,--wrap=ioctl,--wrap=malloc,--wrap=calloc,--wrap=free,--wrap=mmap,--wrap=munmap -o $@ $^

test-gfx-client: $(TEST_BLD)/test_gfx_client.elf
	$<

# Gfx primitives test (Task 4, 2D graphics API plan): host-compiles
# the REAL libgfx/line.c + libgfx/sprite.c (the archive functions
# the spec §4 designates for Bresenham and sprite blits) and links
# them through the in-tree libgfx/internal.h so the test can read
# the opaque handle layout directly.  libgfx/gfx.c is NOT compiled
# here (it would drag open/ioctl/close; the primitives test is
# pure in-memory drawing — no fd, no syscall).
#
# The test exercises every primitive spec §4 contract: pixel /
# hline / vline / rect / fill_rect colour correctness, full
# clipping matrix (negative coords, oversized coords, partial
# overlap, complete outside, clip rectangle), Bresenham in all 8
# octants with both endpoints, INT32_MIN/MAX endpoints that must
# not overflow or loop, sprite opaque / color-key=0 (black also
# transparent) / mask MSB-first bit order / padded source and mask
# stride.  Sentinel pixel 0xDEADBEEF wraps the 7x5 grid so any
# out-of-bounds write shows up in an assert.
$(TEST_BLD)/libgfx_line_production.o: $(TESTS_DIR)/libgfx/line.c \
		$(TESTS_DIR)/libgfx/gfx.h \
		$(TESTS_DIR)/libgfx/internal.h \
		$(TESTS_DIR)/kernel/include/uapi/gfx.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_CLIENT_PROD_CFLAGS) -c $< -o $@

$(TEST_BLD)/libgfx_sprite_production.o: $(TESTS_DIR)/libgfx/sprite.c \
		$(TESTS_DIR)/libgfx/gfx.h \
		$(TESTS_DIR)/libgfx/internal.h \
		$(TESTS_DIR)/kernel/include/uapi/gfx.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_CLIENT_PROD_CFLAGS) -c $< -o $@

# Test TU: include path adds the in-tree libgfx/ for internal.h
# (internal.h is NEVER installed to the sysroot; tests reach it
# directly so they can read the handle layout, which is the same
# layout libgfx/gfx.c, libgfx/line.c and libgfx/sprite.c use).
$(TEST_BLD)/test_gfx_primitives.o: $(TEST_CASES)/test_gfx_primitives.c \
		$(TESTS_DIR)/libgfx/gfx.h \
		$(TESTS_DIR)/libgfx/internal.h \
		$(TESTS_DIR)/kernel/include/uapi/gfx.h \
		$(TEST_INC)/test_framework.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_CLIENT_TEST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_gfx_primitives.elf: $(TEST_BLD)/test_gfx_primitives.o \
		$(TEST_BLD)/libgfx_gfx_production.o \
		$(TEST_BLD)/libgfx_line_production.o \
		$(TEST_BLD)/libgfx_sprite_production.o
	$(HOST_CC) -o $@ $^

test-gfx-primitives: $(TEST_BLD)/test_gfx_primitives.elf
	$<

# setres CLI + long-running gfx client policy (QEMU resolution switcher
# Task 8).  Host-compiles the REAL user/setres/setres_parse.c, user/setres/setres.c
# (with -DSETRES_NO_MAIN so this TU owns main) and
# user/common/gfx_client_policy.c.  libc open/close/ioctl are wrapped so the
# test can assert the CLI's exact device ops (help opens NOTHING) and
# inject GET_MODES / GET_STATE / SET_MODE results.
#
# The errno ABI shim (GFX_CLIENT_*_CFLAGS) is reused verbatim from the
# libgfx client test: kernel/include is on the path first for
# <uapi/fb.h> and its freestanding <errno.h>, so `errno` must be
# redirected to glibc's TLS accessor.  user/ is added for the
# "setres_parse.h" / "gfx_client_policy.h" includes.
SETRES_HOST_CFLAGS := $(GFX_CLIENT_PROD_CFLAGS) -I$(TESTS_DIR)/user/setres -I$(TESTS_DIR)/user/common

$(TEST_BLD)/gfx_client_policy.o: $(TESTS_DIR)/user/common/gfx_client_policy.c \
		$(TESTS_DIR)/user/common/gfx_client_policy.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(SETRES_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/setres_parse.o: $(TESTS_DIR)/user/setres/setres_parse.c \
		$(TESTS_DIR)/user/setres/setres_parse.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(SETRES_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/setres_cli.o: $(TESTS_DIR)/user/setres/setres.c \
		$(TESTS_DIR)/user/setres/setres_parse.h \
		$(TESTS_DIR)/kernel/include/uapi/fb.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(SETRES_HOST_CFLAGS) -DSETRES_NO_MAIN -c $< -o $@

$(TEST_BLD)/test_setres.o: $(TEST_CASES)/test_setres.c \
		$(TESTS_DIR)/user/common/gfx_client_policy.h \
		$(TESTS_DIR)/user/setres/setres_parse.h \
		$(TESTS_DIR)/kernel/include/uapi/fb.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_CLIENT_TEST_CFLAGS) -I$(TESTS_DIR)/user/setres -I$(TESTS_DIR)/user/common -D_GNU_SOURCE -c $< -o $@

$(TEST_BLD)/test_setres.elf: $(TEST_BLD)/test_setres.o \
		$(TEST_BLD)/setres_cli.o \
		$(TEST_BLD)/setres_parse.o \
		$(TEST_BLD)/gfx_client_policy.o
	$(HOST_CC) -Wl,--wrap=open,--wrap=close,--wrap=ioctl -o $@ $^

test-clients: $(TEST_BLD)/test_setres.elf
	$<

# Framebuffer mode control ABI layout test (QEMU resolution switcher Task 1)
$(TEST_BLD)/test_fb_uapi.o: $(TEST_CASES)/test_fb_uapi.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(KERNEL_INC) -I$(TEST_INC) -c $< -o $@

$(TEST_BLD)/test_fb_uapi.elf: $(TEST_BLD)/test_fb_uapi.o
	$(HOST_CC) -o $@ $^

test-fb-uapi: $(TEST_BLD)/test_fb_uapi.elf
	$<

# Framebuffer state coordinator & checked mapping test (QEMU resolution switcher Task 2)
FB_STATE_HOST_CFLAGS := $(HOST_CFLAGS) -DOS01_HOST_TEST -I$(TEST_MOCK) -include $(TEST_MOCK)/fb_resolution_runtime.h $(KERNEL_INC) -I$(TEST_INC)

$(TEST_BLD)/fb_resolution_runtime.o: $(TEST_MOCK)/fb_resolution_runtime.c $(TEST_MOCK)/fb_resolution_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -DOS01_HOST_TEST -I$(TEST_MOCK) $(KERNEL_INC) -I$(TEST_INC) -c $< -o $@

$(TEST_BLD)/fb_state.o: $(TESTS_DIR)/kernel/driver/fb_state.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_STATE_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/fb_map.o: $(TESTS_DIR)/kernel/arch/x86_64/memory/fb_map.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_STATE_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_fb_state.o: $(TEST_CASES)/test_fb_state.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_STATE_HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_fb_state.elf: $(TEST_BLD)/test_fb_state.o $(TEST_BLD)/fb_state.o $(TEST_BLD)/fb_map.o $(TEST_BLD)/fb_resolution_runtime.o
	$(HOST_CC) -lpthread -o $@ $^

test-fb-state: $(TEST_BLD)/test_fb_state.elf
	$<

# Framebuffer writer admission, leases, and raw mmap rollback test (QEMU resolution switcher Task 3)
FB_WRITERS_HOST_CFLAGS := $(HOST_CFLAGS) -DOS01_HOST_TEST -I$(TEST_MOCK) -include $(TEST_MOCK)/fb_resolution_runtime.h $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC)

$(TEST_BLD)/gfx_production.o: $(TESTS_DIR)/kernel/driver/gfx.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_WRITERS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/fb_production.o: $(TESTS_DIR)/kernel/driver/fb.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_WRITERS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/console_production.o: $(TESTS_DIR)/kernel/tty/console.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_WRITERS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_fb_writers.o: $(TEST_CASES)/test_fb_writers.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_WRITERS_HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_fb_writers.elf: $(TEST_BLD)/test_fb_writers.o $(TEST_BLD)/fb_state.o $(TEST_BLD)/fb_map.o $(TEST_BLD)/fb_resolution_runtime.o $(TEST_BLD)/gfx_production.o $(TEST_BLD)/fb_production.o $(TEST_BLD)/console_production.o
	$(HOST_CC) -lpthread -o $@ $^

test-fb-writers: $(TEST_BLD)/test_fb_writers.elf
	$<

# Display ownership coordinator & exclusive writer leases test (Display Ownership Task 1)
$(TEST_BLD)/test_display_owner.o: $(TEST_CASES)/test_display_owner.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_STATE_HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_display_owner.elf: $(TEST_BLD)/test_display_owner.o $(TEST_BLD)/fb_state.o $(TEST_BLD)/fb_map.o $(TEST_BLD)/fb_resolution_runtime.o
	$(HOST_CC) -lpthread -o $@ $^

test-display-owner: $(TEST_BLD)/test_display_owner.elf
	$<

# PCI BGA backend probe and hardware transaction test (QEMU resolution switcher Task 4)
$(TEST_BLD)/bga_production.o: $(TESTS_DIR)/kernel/driver/bga.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_STATE_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/bga_non_x86.o: $(TESTS_DIR)/kernel/driver/bga.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_STATE_HOST_CFLAGS) -DOS01_HOST_TEST_NON_X86 -c $< -o $@

$(TEST_BLD)/test_bga_resolution.o: $(TEST_CASES)/test_bga_resolution.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_STATE_HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_bga_resolution.elf: $(TEST_BLD)/test_bga_resolution.o $(TEST_BLD)/bga_production.o $(TEST_BLD)/fb_state.o $(TEST_BLD)/fb_map.o $(TEST_BLD)/fb_resolution_runtime.o $(TEST_BLD)/pci_match_production.o
	$(HOST_CC) -lpthread -o $@ $^

$(TEST_BLD)/test_bga_non_x86.o: $(TEST_CASES)/test_bga_non_x86.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_STATE_HOST_CFLAGS) $(FRAMEWORK_INC) -DOS01_HOST_TEST_NON_X86 -c $< -o $@

$(TEST_BLD)/test_bga_non_x86.elf: $(TEST_BLD)/test_bga_non_x86.o $(TEST_BLD)/bga_non_x86.o $(TEST_BLD)/fb_state.o $(TEST_BLD)/fb_map.o $(TEST_BLD)/fb_resolution_runtime.o $(TEST_BLD)/pci_match_production.o
	$(HOST_CC) -lpthread -o $@ $^

test-bga: $(TEST_BLD)/test_bga_resolution.elf $(TEST_BLD)/test_bga_non_x86.elf
	$(TEST_BLD)/test_bga_resolution.elf
	$(TEST_BLD)/test_bga_non_x86.elf

# Framebuffer safe ioctl and SET state machine test (QEMU resolution switcher Task 5)
$(TEST_BLD)/test_fb_resolution_ioctl.o: $(TEST_CASES)/test_fb_resolution_ioctl.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_WRITERS_HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_fb_resolution_ioctl.elf: $(TEST_BLD)/test_fb_resolution_ioctl.o $(TEST_BLD)/fb_state.o $(TEST_BLD)/fb_map.o $(TEST_BLD)/fb_resolution_runtime.o $(TEST_BLD)/fb_production.o $(TEST_BLD)/console_production.o $(TEST_BLD)/bga_production.o $(TEST_BLD)/pci_match_production.o
	$(HOST_CC) -lpthread -o $@ $^

test-fb-ioctl: $(TEST_BLD)/test_fb_resolution_ioctl.elf
	$<

# Controlled fault-injection surface (QEMU resolution switcher Task 9).
# Compiles the REAL kernel/driver/fb_test.c together with fb_state.c /
# bga.c / fb.c built with -DFB_RESOLUTION_TEST, so the armed-token
# consumption, readback filtering, lease hold and terminal-ENOMEM
# registry are all exercised end to end against the fake BGA transport.
# fb_state.c / bga.c / fb.c get distinct object names because their
# normal FB_STATE/FB_WRITERS objects are built WITHOUT the macro.
FB_HOOKS_HOST_CFLAGS := $(FB_WRITERS_HOST_CFLAGS) -DFB_RESOLUTION_TEST

$(TEST_BLD)/fb_state_hooks.o: $(TESTS_DIR)/kernel/driver/fb_state.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_HOOKS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/bga_hooks.o: $(TESTS_DIR)/kernel/driver/bga.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_HOOKS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/fb_test.o: $(TESTS_DIR)/kernel/driver/fb_test.c \
		$(TEST_MOCK)/fb_resolution_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_HOOKS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_fb_resolution_hooks.o: $(TEST_CASES)/test_fb_resolution_hooks.c \
		$(TEST_MOCK)/fb_resolution_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_HOOKS_HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_fb_resolution_hooks.elf: $(TEST_BLD)/test_fb_resolution_hooks.o \
		$(TEST_BLD)/fb_state_hooks.o $(TEST_BLD)/bga_hooks.o \
		$(TEST_BLD)/fb_test.o $(TEST_BLD)/fb_map.o \
		$(TEST_BLD)/fb_resolution_runtime.o $(TEST_BLD)/fb_production.o \
		$(TEST_BLD)/console_production.o $(TEST_BLD)/pci_match_production.o
	$(HOST_CC) -lpthread -o $@ $^

test-fb-hooks: $(TEST_BLD)/test_fb_resolution_hooks.elf
	$<

