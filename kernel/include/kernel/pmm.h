#ifndef _KERNEL_PMM_H
#define _KERNEL_PMM_H

#include <stdint.h>

#define PAGE_1G_SHIFT  30
#define PAGE_2M_SHIFT  21
#define PAGE_4K_SHIFT  12

#define PAGE_2M_SIZE (1UL << PAGE_2M_SHIFT)
#define PAGE_4K_SIZE (1UL << PAGE_4K_SHIFT)

#define PAGE_2M_MASK	(~ (PAGE_2M_SIZE - 1))
#define PAGE_4K_MASK	(~ (PAGE_4K_SIZE - 1))

#define PAGE_2M_ALIGN(addr) (((unsigned long)(addr) + PAGE_2M_SIZE - 1) & PAGE_2M_MASK)
#define PAGE_4K_ALIGN(addr)	(((unsigned long)(addr) + PAGE_4K_SIZE - 1) & PAGE_4K_MASK)
#define ZONE_DMA      (1 << 0)
#define ZONE_NORMAL   (1 << 1)
#define ZONE_UNMAPPED (1 << 2)

// mapped=1, un-mapped=0
#define PG_PTable_Mapped (1 << 0)
// init-code=1, normal-code/data=0
#define PG_Kernel_Init   (1 << 1)
// device=1, memory=0
#define PG_Device        (1 << 2)
// kernel=1, user=0
#define PG_Kernel        (1 << 3)
// shared=1, single-sue=0
#define PG_Shared        (1 << 4)

struct E820
{
    uint64_t address;
    uint64_t length;
    uint8_t type;
}__attribute__((packed));

struct Physical_Memory_Manager{
    struct E820 e820_entrys[32];
    uint64_t e820_length;

    uint64_t * bits_map;
    uint64_t   bits_size;
    uint64_t   bits_length;

    struct Page * pages_struct;
    uint64_t pages_size;
    uint64_t pages_length;

    struct Zone * zones_struct;
    uint64_t zones_size;
    uint64_t zones_length;

    uint64_t start_code, end_code, end_data, end_rodata, start_brk;
    
    uint64_t end_of_struct;
};

struct Page
{
    struct Zone * zone_struct;
    uint64_t phy_address;
    uint64_t attribute;
    uint64_t reference_count;
    uint64_t age;
};

uint32_t ZONE_DMA_INDEX = 0;
uint32_t ZONE_NORMAL_INDEX = 0;   //low 1GB RAM, mapped in pagetable
uint32_t ZONE_UNMAPPED_INDEX = 0; //above 1GB RAM, unmapped in pagetable

#define MAX_NR_ZONES 10 //max zone

struct Zone
{
    struct Page * pages_group;
    uint64_t pages_length;

    uint64_t zone_start_address;
    uint64_t zone_end_address;
    uint64_t zone_length;
    uint64_t attribute;

    struct Physical_Memory_Manager * manager_struct;

    uint64_t page_using_count;
    uint64_t page_free_count;

    uint64_t total_pages_link;
};

#endif