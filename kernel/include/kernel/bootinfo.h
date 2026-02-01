#ifndef _KERNEL_BOOTINFO_H
#define _KERNEL_BOOTINFO_H

struct GRAPHICS_INFO
{
	unsigned int HorizontalResolution;
	unsigned int VerticalResolution;
	unsigned int PixelsPerScanLine;

	unsigned long FrameBufferBase;
	unsigned long FrameBufferSize;
};

struct E820_ENTRY
{
	unsigned long address;
	unsigned long length;
	unsigned int  type;
}__attribute__((packed));

struct MEMORY_INFO
{
	unsigned int E820_Entry_count;
	struct E820_ENTRY * E820_Entry;
};

struct BOOT_INFO
{
	struct GRAPHICS_INFO Graphics_Info;
	struct MEMORY_INFO E820_Info;
    unsigned long long RSDP;
    unsigned char BootFromBIOS;
};

#endif