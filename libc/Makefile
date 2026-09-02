# ── Architecture ────────────────────────────────────────────
HOSTARCH  := x86_64
ARCHDIR   := arch/$(HOSTARCH)
ARCH ?= x86_64
LOCAL_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))

# ── Profile mode (root os01_submake) vs standalone legacy ──
# With OS01_PROFILE_FILE set, include the selected profile instead of the
# legacy toolchain.mk and take the build dir from the profile
# (LIBC_BUILD_DIR). The two paths are mutually exclusive.
ifdef OS01_PROFILE_FILE
include $(OS01_PROFILE_FILE)
BUILD_DIR := $(LIBC_BUILD_DIR)
else
BUILD_DIR := ../build/$(HOSTARCH)/libc
ifeq ($(ARCH),x86_64)
include $(dir $(LOCAL_MAKEFILE))../toolchain.mk
endif
endif

# ── Load architecture config ────────────────────────────────
include $(ARCHDIR)/make.config

# ── Compile flags ──────────────────────────────────────────
CFLAGS := $(CFLAGS) -ffreestanding -Wall -Wextra -D__is_libc -Iinclude -fno-stack-protector -fomit-frame-pointer $(ARCH_CFLAGS)
LIBK_CFLAGS := $(CFLAGS) -D__is_libk -mno-red-zone -I../kernel/include

# ── Source discovery (wildcard — no manual lists!) ──────────
C_SOURCES := \
    $(wildcard ctype/*.c) \
    $(wildcard errno/*.c) \
    $(wildcard list/*.c) \
    $(wildcard rbtree/*.c) \
    $(wildcard network/*.c) \
    $(wildcard stdio/*.c) \
    $(wildcard stdlib/*.c) \
    $(wildcard string/*.c) \
    $(wildcard time/*.c) \
    $(wildcard unistd/*.c) \
    $(wildcard termios/*.c) \
    $(wildcard socket/*.c) \
    $(wildcard netdb/*.c) \
    $(wildcard sched/*.c) \
    $(wildcard xattr/*.c) \
    $(wildcard resource/*.c) \
    $(wildcard pwd/*.c) \
    $(wildcard grp/*.c) \
    $(wildcard csu/*.c) \
    $(wildcard signal/*.c) \
    $(wildcard arpa/*.c) \
    $(wildcard libgen/*.c) \
    $(wildcard pthread/*.c) \
    $(wildcard random/*.c)

# Remove duplicates: free.c conflicts with malloc.c (chown.c conflict gone — uid_stubs.c deleted)
C_SOURCES := $(filter-out stdlib/free.c, $(C_SOURCES))

# Object files (inside BUILD_DIR)
# string/memcmp.c → build/x86_64/libc/string/memcmp.o     (libc)
#                  → build/x86_64/libc/string/memcmp.libk.o (libk)
LIBC_OBJS := $(addprefix $(BUILD_DIR)/, $(C_SOURCES:.c=.o))
LIBK_OBJS := $(addprefix $(BUILD_DIR)/, $(C_SOURCES:.c=.libk.o))

# Arch-specific objects (if any)
LIBC_ARCH_OBJS := $(addprefix $(BUILD_DIR)/, $(ARCH_FREEOBJS))
LIBK_ARCH_OBJS := $(addprefix $(BUILD_DIR)/, $(ARCH_FREEOBJS:.o=.libk.o))
LIBC_HOSTED_OBJS := $(addprefix $(BUILD_DIR)/, $(ARCH_HOSTEDOBJS))

LIBC_ALL_OBJS := $(LIBC_OBJS) $(LIBC_ARCH_OBJS) $(LIBC_HOSTED_OBJS)
LIBK_ALL_OBJS := $(LIBK_OBJS) $(LIBK_ARCH_OBJS)

# ── Targets ────────────────────────────────────────────────
.PHONY: all clean install install-headers install-libs

all: libc.a libk.a

libc.a: $(LIBC_ALL_OBJS)
	$(AR) rcs $@ $(LIBC_ALL_OBJS)

libk.a: $(LIBK_ALL_OBJS)
	$(AR) rcs $@ $(LIBK_ALL_OBJS)

# ── Compile rules ──────────────────────────────────────────
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(EFFECTIVE_CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.libk.o: %.c
	@mkdir -p $(dir $@)
	$(EFFECTIVE_CC) $(LIBK_CFLAGS) -MMD -MP -c $< -o $@

# ── Installation (into sysroot) ────────────────────────────
install: install-headers install-libs

install-headers:
	mkdir -p $(TARGET_INCLUDEDIR)
	cp -R --preserve=timestamps include/. $(TARGET_INCLUDEDIR)/.

install-libs: libc.a libk.a
	mkdir -p $(TARGET_LIBDIR)
	@cmp -s libc.a $(TARGET_LIBDIR)/libc.a || cp libc.a $(TARGET_LIBDIR)/libc.a
	@cmp -s libk.a $(TARGET_LIBDIR)/libk.a || cp libk.a $(TARGET_LIBDIR)/libk.a

# ── Clean ──────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_DIR) libc.a libk.a

# ── Dependency tracking ────────────────────────────────────
ALL_DEPS := $(patsubst %.o,%.d,$(LIBC_ALL_OBJS)) $(patsubst %.o,%.d,$(LIBK_ALL_OBJS))
-include $(ALL_DEPS)
