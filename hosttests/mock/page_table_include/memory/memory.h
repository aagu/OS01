#ifndef HOST_MEMORY_H
#define HOST_MEMORY_H
#include_next <memory/memory.h>
#undef Phy_To_Virt
#undef Virt_To_Phy
#define Phy_To_Virt(addr) ((unsigned long *)(uintptr_t)(addr))
#define Virt_To_Phy(addr) ((unsigned long)(addr))
#endif
