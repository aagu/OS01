#ifndef _KERNEL_MEMORY_H
#define _KERNEL_MEMORY_H

#include <stdint.h>
#include <kernel/bootinfo.h>

void pmm_init(struct MEMORY_INFO E820_Info);

#endif