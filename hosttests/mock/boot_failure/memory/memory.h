#ifndef _MOCK_BOOT_FAILURE_MEMORY_H
#define _MOCK_BOOT_FAILURE_MEMORY_H

#include_next <memory/memory.h>
#undef Phy_To_Virt
#define Phy_To_Virt(addr) ((void *)(uintptr_t)(addr))
#undef Virt_To_Phy
#define Virt_To_Phy(addr) ((uint64_t)(uintptr_t)(addr))

#endif
