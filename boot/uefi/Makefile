TARGET = BOOTX64.EFI

USE_GCC=1
include uefi/Makefile

OVMF.fd:
	wget -c https://retrage.github.io/edk2-nightly/bin/RELEASEX64_OVMF.fd -O $@