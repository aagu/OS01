#ifndef _KERNEL_BOOTINFO_H
#define _KERNEL_BOOTINFO_H

#include <stdint.h>

// All fields use fixed-size types (uint32_t, uint64_t) to ensure
// identical layout regardless of data model (LP64 vs LLP64).
// This matters because the EFI bootloader may be compiled with a
// different toolchain (clang --target=x86_64-pc-win32-coff) that
// uses 4-byte 'unsigned long', while the kernel uses 8-byte.

struct GRAPHICS_INFO
{
	uint32_t HorizontalResolution;
	uint32_t VerticalResolution;
	uint32_t PixelsPerScanLine;

	uint64_t FrameBufferBase;
	uint64_t FrameBufferSize;
};

struct E820_ENTRY
{
	uint64_t address;
	uint64_t length;
	uint32_t type;
}__attribute__((packed));

struct MEMORY_INFO
{
	uint32_t E820_Entry_count;
	uint64_t E820_Entry;  // physical address of E820 array
};

struct BOOT_INFO
{
	struct GRAPHICS_INFO Graphics_Info;
	struct MEMORY_INFO E820_Info;
	uint64_t RSDP;
	uint8_t  BootFromBIOS;
};

#endif