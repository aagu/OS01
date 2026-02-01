TARGET = BOOTX64.EFI

USE_GCC=1
ARCH=x86_64
ifeq ($(DEBUG),1)
CFLAGS += -DDEBUG=1
endif
include uefi/Makefile

OVMF.fd:
	wget -c https://retrage.github.io/edk2-nightly/bin/RELEASEX64_OVMF.fd -O $@