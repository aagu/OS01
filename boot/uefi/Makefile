# boot/uefi/Makefile — UEFI bootloader
#
# Build logic is delegated to the posix-uefi submodule (thirdpart/posix-uefi/).
# This Makefile wraps it with a clean target and OVMF download.

TARGET = BOOTX64.EFI

#USE_GCC=1
ARCH=x86_64
ifeq ($(DEBUG),1)
CFLAGS += -DDEBUG=1
endif
# The UEFI link step uses `lld -flavor link`, which does not understand the
# --sysroot flag (emitted for the kernel/user toolchain). Strip it so lld
# does not warn about an unknown argument.
LDFLAGS := $(filter-out --sysroot=%,$(LDFLAGS))
# Create archives without the benign "creating libuefi.a" notice.
AR := llvm-ar -c

uefi_mk := uefi/Makefile
ifneq ($(wildcard $(uefi_mk)),)
include $(uefi_mk)
else
$(error "posix-uefi submodule not initialized. Run 'git submodule update --init'")
endif

.PHONY: all clean clean-all

all: $(TARGET)

OVMF.fd:
	wget -c https://retrage.github.io/edk2-nightly/bin/RELEASEX64_OVMF.fd -O $@

# `clean` itself is provided by the included uefi/Makefile; we only add the
# wrapper-level cleanup here so the two do not redefine the same target.
clean-all:
	rm -f *.o *.EFI *.efi *.bin
	rm -rf build/
	rm -f OVMF.fd
