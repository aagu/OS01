# ── Architecture ────────────────────────────────────────────
# ARCH is supplied by the root artifact rules (mk/components/kernel.mk for
# x86_64, mk/components/image.mk for aarch64) — the kernel never selects an
# architecture by default.
ARCHDIR     := arch/$(ARCH)

# ── Profile-only contract ───────────────────────────────────
# The root Makefile is the only supported entry; component Makefiles
# require OS01_PROFILE_FILE at parse time. Build dir and toolchain come
# from the selected profile (KERNEL_BUILD_DIR).
LOCAL_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))
ifeq ($(strip $(OS01_PROFILE_FILE)),)
$(error invoke from the repository root: make PROFILE=x86_64-clang <target>)
endif
ifeq ($(wildcard $(OS01_PROFILE_FILE)),)
$(error OS01_PROFILE_FILE='$(OS01_PROFILE_FILE)' does not exist; invoke from the repository root)
endif
include $(OS01_PROFILE_FILE)
BUILD_DIR := $(KERNEL_BUILD_DIR)

# ── Load architecture configuration ────────────────────────
include $(ARCHDIR)/make.config

# Toolchain picks are architecture-specific and live in arch/<arch>/make.config.
# Keep the generic kernel Makefile free from arch-specific compiler/linker logic.
# x86_64 gets EFFECTIVE_CC/OBJ_CPY from the profile's clang.mk; aarch64 falls
# back to its own CC (from make.config) and llvm-objcopy here.
EFFECTIVE_CC ?= $(CC)
OBJ_CPY ?= llvm-objcopy

# Kernel headers path (relative to kernel/ CWD)
KERNEL_HEADERS := include

# ── Source discovery via wildcard (no manual lists!) ───────
# All paths are relative to kernel/ (CWD after make -C kernel)
#
# Arch gate: aarch64 phase 1 starts from an empty whitelist (linker pulls
# in arch/aarch64/*.c via ARCH_C_SOURCES below). x86_64 keeps the full
# legacy wildcard set. Sources are added back to the aarch64 whitelist
# incrementally as the port grows (e.g. arch-neutral helpers), gated by
# "what the linker complains about".
ifeq ($(ARCH),aarch64)
KERNEL_C_SOURCES :=
else
KERNEL_C_SOURCES := \
    $(wildcard kernel/*.c) \
    $(wildcard net/*.c) \
    $(wildcard memory/*.c) \
    $(wildcard fs/*.c) \
    $(wildcard sched/*.c) \
    $(wildcard sync/*.c) \
    $(wildcard driver/*.c) \
    $(wildcard tty/*.c) \
    $(wildcard intr/*.c) \
    $(wildcard block/*.c) \
    $(wildcard time/*.c) \
    $(wildcard percpu/*.c) \
    $(wildcard test/*.c) \
    $(wildcard subsys/*.c)

# Platform devices are selected by arch/<arch>/make.config.  Keep x86-only
# APIC/PIC/PIT/RTC code out of architecture-neutral source discovery.
KERNEL_C_SOURCES := $(filter-out driver/pit.c driver/rtc.c,$(KERNEL_C_SOURCES))
KERNEL_C_SOURCES += $(ARCH_PLATFORM_C_SOURCES)
endif

# Architecture C sources and asm sources
ARCH_C_SOURCES  := $(wildcard $(ARCHDIR)/*.c)
ARCH_ASM_SOURCES := $(wildcard $(ARCHDIR)/*.S)
# Exclude trampoline.S from normal compilation (special handling below)
ARCH_ASM_SOURCES := $(filter-out $(ARCHDIR)/trampoline.S, $(ARCH_ASM_SOURCES))

ALL_C_SOURCES   := $(KERNEL_C_SOURCES) $(ARCH_C_SOURCES)
# kallsyms is a two-pass build artifact, not a regular source
ALL_C_SOURCES   := $(filter-out kernel/kallsyms.c, $(ALL_C_SOURCES))
ALL_S_SOURCES   := $(ARCH_ASM_SOURCES)

# ── Object files (under BUILD_DIR) ─────────────────────────
# Path mapping:  kernel/main.c    → build/x86_64/kernel/main.o
#                memory/pmm.c     → build/x86_64/memory/pmm.o
#                arch/x86_64/trap.c → build/x86_64/arch/x86_64/trap.o
C_OBJECTS       := $(patsubst %.c,$(BUILD_DIR)/%.o,$(ALL_C_SOURCES))
ASM_OBJECTS     := $(patsubst %.S,$(BUILD_DIR)/%.o,$(ALL_S_SOURCES))

KERNEL_EXTRA_OBJECTS ?=

KERNEL_OBJECTS := $(C_OBJECTS) $(ASM_OBJECTS) $(KERNEL_EXTRA_OBJECTS)

# ── Flags ──────────────────────────────────────────────────
ALL_CFLAGS   := $(CFLAGS) $(ARCH_CFLAGS)   -I$(KERNEL_HEADERS)
ifeq ($(ARCH),x86_64)
# The kernel consumes libc headers (<string.h>, <stdlib.h>, ...) from
# the immutable sysroot generation it is compiled against. SYSROOT_GENERATION_DIR
# is passed by the root artifact rule (mk/components/kernel.mk) under the
# generation lease. Guarded by ARCH so aarch64 profile mode never gets an
# empty -isystem.
ALL_CFLAGS += -isystem $(SYSROOT_GENERATION_DIR)/usr/include
endif
ALL_ASMFLAGS := $(ASFLAGS) $(ARCH_ASMFLAGS)
ifeq ($(ARCH),x86_64)
# The kernel links -lk against the immutable sysroot generation.
ALL_LDFLAGS  := $(LDFLAGS) $(ARCH_LDFLAGS) -L$(SYSROOT_GENERATION_DIR)/usr/lib
else
ALL_LDFLAGS  := $(LDFLAGS) $(ARCH_LDFLAGS) -L$(SYSROOT)/usr/lib
endif
LIBS         := $(ARCH_LIBS)

# ── DEBUG / NDEBUG (align with busybox convention) ────────
# Release (default): compile out log_debug() via -DNDEBUG, -O2.
#   make DEBUG=1  → debug: keep log_debug(), -O0 for GDB.
ifneq ($(DEBUG),1)
ALL_CFLAGS += -DNDEBUG
ALL_CFLAGS += -O2     # Release: shrink stack frames significantly
else
ALL_CFLAGS += -DDEBUG=1
ALL_CFLAGS += -O0     # Debug: easy GDB single-stepping
endif

# ── lwIP networking stack ────────────────────────────────────
LWIP_SRC_DIR  := ../thirdpart/lwip/src
LWIP_CORE     := $(wildcard $(LWIP_SRC_DIR)/core/*.c)
LWIP_CORE4    := $(wildcard $(LWIP_SRC_DIR)/core/ipv4/*.c)
LWIP_NETIF    := $(wildcard $(LWIP_SRC_DIR)/netif/*.c)
LWIP_API      := $(wildcard $(LWIP_SRC_DIR)/api/*.c)

# Exclude PPP/SLIP/SLIPIF/ethernetif.c (we provide our own netif)
LWIP_NETIF    := $(filter-out %/ppp.c %/slipif.c %/ethernetif.c, $(LWIP_NETIF))

LWIP_SOURCES  := $(LWIP_CORE) $(LWIP_CORE4) $(LWIP_NETIF) $(LWIP_API)
# Map lwIP sources to build dir: thirdpart/lwip/src/core/tcp.c → build/.../lwip/core/tcp.o
LWIP_OBJECTS  := $(patsubst $(LWIP_SRC_DIR)/%.c,$(BUILD_DIR)/lwip/%.o,$(LWIP_SOURCES))

# lwIP is enabled only on architectures that declare it.
ifneq ($(ARCH_ENABLE_LWIP),0)
KERNEL_OBJECTS += $(LWIP_OBJECTS)
endif

# ── stdint.h injection (see net/arch/cc.h) ─────────────────
# Clang's freestanding builtin <stdint.h> typedefs uint64_t =
# __UINT64_TYPE__ (= `unsigned long` on LP64 x86_64).  OS01's
# libc/include/stdint.h (and net/arch/cc.h by extension) typedef
# uint64_t = `unsigned long long` so kernel+userspace ABIs match
# across syscall / lwIP struct boundaries (sockets, timeval, msghdr).
# Without these two flags, every kernel TU that #includes <stdint.h>
# before net/arch/cc.h hits:
#   typedef redefinition with different types ('unsigned long long'
#   vs 'unsigned long')
# (-D__CLANG_STDINT_H skips Clang's whole builtin; -include forces
#  sysroot's libc stdint.h to be parsed first, so the canonical
#  OS01 libc widths win everywhere in the kernel.)
#
# Stored in ALL_C_ONLY_FLAGS (not ALL_CFLAGS) because the .S → .o
# rule assembles through the C preprocessor, which can't parse C
# typedefs and would choke on libc's stdint.h.
ALL_CFLAGS     += -I$(LWIP_SRC_DIR)/include -I../kernel/include/net
ifeq ($(ARCH),x86_64)
# Kernel-profile stdint injection, resolved against the immutable generation
# (profile's KERNEL_STDINT_FLAGS). Guarded by ARCH so aarch64 profile mode
# makes no SYSROOT_GENERATION_DIR reference.
ALL_C_ONLY_FLAGS := $(KERNEL_STDINT_FLAGS)
endif
ALL_LDFLAGS    +=
ifdef DEBUG_CHANNELS
  comma := ,
  empty :=
  space := $(empty) $(empty)
  CHANNELS := $(subst $(comma),$(space),$(DEBUG_CHANNELS))
  ALL_CFLAGS += $(foreach ch,$(CHANNELS),-DOS01_DEBUG_$(ch)=1)
endif

# ── Built-in selftests (make KERNEL_SELFTEST=1) ────────────────
ifdef KERNEL_SELFTEST
ALL_CFLAGS += -DOS01_SELFTEST=1
endif

# ── Log output target (serial | fb | both) ────────────────
# Helper: non-empty → 1, empty → 0
bool = $(if $1,1,0)

LOG_TARGET ?= serial
ALL_CFLAGS += -DLOG_TARGET_SERIAL=$(call bool,$(filter serial both,$(LOG_TARGET)))
ALL_CFLAGS += -DLOG_TARGET_FB=$(call bool,$(filter fb both,$(LOG_TARGET)))

# ── Targets ────────────────────────────────────────────────
.PHONY: all clean install-headers

all: $(BUILD_DIR)/kernel.bin
# The root artifact rule (mk/components/kernel.mk) copies artifacts/kernel.bin
# from the profile build dir; do not copy back into the source tree.
	@true

# ── Compilation rules ──────────────────────────────────────
# .c → .o + .d
# C-only flags carry the stdint.h injection (see CFLAGS comment above);
# ASM files (.S) get ALL_CFLAGS only since the assembler preprocessor
# can't parse a C typedef.
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(EFFECTIVE_CC) $(ALL_CFLAGS) $(ALL_C_ONLY_FLAGS) -MMD -MP -c $< -o $@

# .S → .o + .d
$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(EFFECTIVE_CC) $(ALL_CFLAGS) -MMD -MP -c $< -o $@

# ── lwIP compilation ──────────────────────────────────────────
# lwIP sources live outside kernel/ under thirdpart/lwip/src
$(BUILD_DIR)/lwip/%.o: $(LWIP_SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(EFFECTIVE_CC) $(ALL_CFLAGS) $(ALL_C_ONLY_FLAGS) -MMD -MP -c $< -o $@

# ── Special objects ────────────────────────────────────────
# PSF font → .o
ifeq ($(ARCH),x86_64)
$(BUILD_DIR)/kernel/font.o: kernel/font.psf
	@mkdir -p $(dir $@)
	$(TARGET_LD) $(RAW_LD_EMULATION) -r -b binary -o $@ $<

# Trampoline: assemble → link at 0x8000 → binary → embed
$(BUILD_DIR)/$(ARCHDIR)/trampoline.o: $(ARCHDIR)/trampoline.S
	@mkdir -p $(dir $@)
	$(EFFECTIVE_CC) $(ALL_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/$(ARCHDIR)/trampoline.elf: $(BUILD_DIR)/$(ARCHDIR)/trampoline.o $(ARCHDIR)/trampoline.ld
	$(TARGET_LD) $(RAW_LD_EMULATION) -T $(ARCHDIR)/trampoline.ld $< -o $@

$(BUILD_DIR)/$(ARCHDIR)/trampoline.bin: $(BUILD_DIR)/$(ARCHDIR)/trampoline.elf
	$(OBJ_CPY) -O binary $< $@

# The intermediate binary is generated under the profile build dir (never
# written into the source tree). The kernel's trampoline.h hardcodes the
# linker-derived symbols _binary_arch_x86_64_trampoline_bin_*, which depend
# on the *path string* passed to `ld -r -b binary`. Run the embed link from
# $(KERNEL_BUILD_DIR)/generated with the relative path
# arch/x86_64/trampoline.bin so lld emits exactly those symbol names.
$(BUILD_DIR)/$(ARCHDIR)/trampoline_bin.o: $(BUILD_DIR)/$(ARCHDIR)/trampoline.bin
	@mkdir -p $(KERNEL_BUILD_DIR)/generated/arch/x86_64
	cp $< $(KERNEL_BUILD_DIR)/generated/arch/x86_64/trampoline.bin
	cd $(KERNEL_BUILD_DIR)/generated && $(TARGET_LD) $(RAW_LD_EMULATION) -r -b binary arch/x86_64/trampoline.bin -o $@
endif

# ── Temporary: irq.o avoids .bss section issue ─────────────
$(BUILD_DIR)/intr/irq.o: intr/irq.c
	@mkdir -p $(dir $@)
	$(EFFECTIVE_CC) $(ALL_CFLAGS) -MMD -MP -c $< -o $@

# ── Kallsyms (two-pass link) ───────────────────────────────
# Host build tools for the kallsyms two-pass link (host-only; no target flags).
HOST_CC ?= clang
HOST_NM ?= nm

# Stage 1: link without kallsyms, just to get symbol table
$(BUILD_DIR)/kernel.elf.stage1: $(KERNEL_OBJECTS) $(ARCHDIR)/$(ARCH_LINKER) $(KERNEL_RUNTIME_PREREQ)
	@set -eu; \
	partial="$(KERNEL_RUNTIME_LINK_RECEIPT).stage1"; \
	tmp="$$partial.tmp.$$$$"; \
	mkdir -p "$$(dirname "$$partial")"; \
	trap 'rm -f "'"$$tmp"'"' EXIT; \
	write_command() { \
	  label="$$1"; shift; printf '%s=' "$$label"; \
	  for arg do escaped=$$(printf '%s' "$$arg" | sed "s/'/'\\\\''/g"); printf " '%s'" "$$escaped"; done; \
	  printf '\n'; \
	}; \
	set -- $(EFFECTIVE_CC) $(ALL_LDFLAGS) -o $@ $(KERNEL_OBJECTS) -T $(ARCHDIR)/$(ARCH_LINKER) $(LIBS) $(KERNEL_RUNTIME_INPUTS); \
	"$$@"; \
	write_command stage1 "$$@" > "$$tmp"; \
	mv "$$tmp" "$$partial"

# Generate kallsyms object from stage1's nm output
$(BUILD_DIR)/kernel/kallsyms.o: kernel/kallsyms.c $(BUILD_DIR)/kernel.elf.stage1
	@mkdir -p $(dir $@)
# kallsyms.c is a host build tool, so it must not pick up the OS01
# sysroot flags from the root Makefile.  Its generated kallsyms.S,
# however, is linked into the kernel and must be assembled with the
# selected target compiler.  Strip exported CFLAGS/LDFLAGS in both
# cases, while retaining the target triple for kallsyms.o.
	env -u CFLAGS -u LDFLAGS $(HOST_CC) -o $(BUILD_DIR)/kernel/kallsyms kernel/kallsyms.c
	$(HOST_NM) -n $(BUILD_DIR)/kernel.elf.stage1 | $(BUILD_DIR)/kernel/kallsyms > $(BUILD_DIR)/kernel/kallsyms.S
	env -u CFLAGS -u LDFLAGS $(EFFECTIVE_CC) -c -o $@ $(BUILD_DIR)/kernel/kallsyms.S
# NB: kernel.elf.stage1 is deliberately KEPT — it is the incremental trigger
# of the kallsyms pass.  The two-pass re-runs only when an object (or the
# linker script) changes, so a no-op `make kernel.bin` does not relink.

# ── Final link ──────────────────────────────────────────────
$(BUILD_DIR)/kernel.elf: $(KERNEL_OBJECTS) $(BUILD_DIR)/kernel/kallsyms.o $(ARCHDIR)/$(ARCH_LINKER) $(KERNEL_RUNTIME_PREREQ)
ifeq ($(ARCH),x86_64)
	@set -eu; \
	partial="$(KERNEL_RUNTIME_LINK_RECEIPT).stage1"; \
	receipt="$(KERNEL_RUNTIME_LINK_RECEIPT)"; \
	tmp="$$receipt.tmp.$$$$"; \
	trap 'rm -f "'"$$tmp"'"' EXIT; \
	if ! test -f "$$partial" || ! test "$$(wc -l < "$$partial")" -eq 1 || ! grep -q '^stage1=.' "$$partial"; then \
	  echo "ERROR: missing or invalid stage1 kernel link receipt: $$partial" >&2; exit 1; \
	fi; \
	if ! tr -d '\000' < "$$partial" | cmp -s - "$$partial"; then \
	  echo "ERROR: stage1 kernel link receipt contains a NUL byte: $$partial" >&2; exit 1; \
	fi; \
	write_command() { \
	  label="$$1"; shift; printf '%s=' "$$label"; \
	  for arg do escaped=$$(printf '%s' "$$arg" | sed "s/'/'\\\\''/g"); printf " '%s'" "$$escaped"; done; \
	  printf '\n'; \
	}; \
	set -- $(TARGET_LD) $(KERNEL_RAW_LDFLAGS) $(KERNEL_RAW_LIBDIR) -o $@ $(KERNEL_OBJECTS) $(BUILD_DIR)/kernel/kallsyms.o \
	    -T $(ARCHDIR)/$(ARCH_LINKER) -lk $(KERNEL_RUNTIME_INPUTS); \
	"$$@"; \
	cat "$$partial" > "$$tmp"; \
	write_command final "$$@" >> "$$tmp"; \
	mv "$$tmp" "$$receipt"
else
	$(EFFECTIVE_CC) $(ALL_LDFLAGS) -o $@ $(KERNEL_OBJECTS) $(BUILD_DIR)/kernel/kallsyms.o \
	    -T $(ARCHDIR)/$(ARCH_LINKER) $(LIBS)
endif

$(BUILD_DIR)/kernel.bin: $(BUILD_DIR)/kernel.elf
	$(OBJ_CPY) -I $(OBJFORMAT) -S -R ".eh_frame" -R ".comment" \
	    -O binary $< $@

# Copy kernel.bin to project root via the "all" target above
.PHONY: kernel.bin
kernel.bin: all
	@true

# ── Install headers ────────────────────────────────────────
install-headers:
	@test -n "$(INSTALL_ROOT)" || { echo "ERROR: install-headers (profile mode) requires INSTALL_ROOT (staging tree)"; exit 1; }
	@# Rebuild the staging tree from source so headers removed from
	@# kernel/include/ do not linger in staging (spec: deleted/renamed
	@# headers must vanish from the next generation).
	rm -rf $(INSTALL_ROOT)
	mkdir -p $(INSTALL_ROOT)/usr/include
	cp -R --preserve=timestamps include/. $(INSTALL_ROOT)/usr/include/.
	@# kernel/include/errno.h is resolved by the kernel build through its own
	@# -Iinclude; in the SHARED sysroot, errno.h is owned by libc (userspace
	@# ABI). Exclude it from the staged kernel headers so the generation
	@# assembler never sees a duplicate destination.
	rm -f $(INSTALL_ROOT)/usr/include/errno.h
	@find $(INSTALL_ROOT) -type f ! -name manifest | sed 's|^$(INSTALL_ROOT)/||' | sort > $(INSTALL_ROOT)/manifest

# ── Clean ───────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_DIR)

# ── Auto-dependency tracking ───────────────────────────────
ALL_DEPS := $(patsubst %.o,%.d,$(C_OBJECTS))
-include $(ALL_DEPS)
