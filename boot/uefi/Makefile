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
include uefi/Makefile

.PHONY: all clean

all: $(TARGET)

OVMF.fd:
	wget -c https://retrage.github.io/edk2-nightly/bin/RELEASEX64_OVMF.fd -O $@

# Clean submodule build artifacts (they live in the source tree since the
# submodule builds in-place)
clean:
	rm -f *.o *.EFI *.efi *.bin
	rm -f OVMF.fd
	rm -rf build/
