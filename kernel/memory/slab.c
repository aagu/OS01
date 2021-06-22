#include <kernel/slab.h>
#include <kernel/memory.h>
#include <kernel/printk.h>
#include <kernel.h>
#include <string.h>

struct Slab_Cache kmalloc_cache_size[16] = 
{
    {32     , 0, 0, NULL, NULL, NULL, NULL},
    {64     , 0, 0, NULL, NULL, NULL, NULL},
    {128    , 0, 0, NULL, NULL, NULL, NULL},
    {256    , 0, 0, NULL, NULL, NULL, NULL},
    {512    , 0, 0, NULL, NULL, NULL, NULL},
    {1024   , 0, 0, NULL, NULL, NULL, NULL},    //1KB
    {2048   , 0, 0, NULL, NULL, NULL, NULL},
    {4096   , 0, 0, NULL, NULL, NULL, NULL},    //4KB
    {8192   , 0, 0, NULL, NULL, NULL, NULL},
    {16384  , 0, 0, NULL, NULL, NULL, NULL},
    {32768  , 0, 0, NULL, NULL, NULL, NULL},
    {65536  , 0, 0, NULL, NULL, NULL, NULL},    //64KB
    {131072 , 0, 0, NULL, NULL, NULL, NULL},
    {262144 , 0, 0, NULL, NULL, NULL, NULL},
    {524288 , 0, 0, NULL, NULL, NULL, NULL},
    {1048576, 0, 0, NULL, NULL, NULL, NULL},    //1MB
};

struct Slab * kmalloc_create(uint64_t size)
{
    uint32_t i;
    struct Slab * slab = NULL;
    struct Page * page = NULL;
    // uint64_t * vaddress = NULL;
    int64_t structsize = 0;

    page = alloc_pages(ZONE_NORMAL,1,0);

    if (page == NULL)
    {
        color_printk(RED,BLACK,"kmalloc_create()->alloc_pages()=>page == NULL\n");
		return NULL;
    }
    
    page_init(page, PG_Kernel);

    switch (size)
    {
    case 32:
    case 64:
    case 128:
    case 256:
    case 512:
        structsize = sizeof(struct Slab) + PAGE_2M_SIZE / size / 8;

        slab = (struct Slab *)((uint8_t *)page->phy_address + PAGE_2M_SIZE - structsize);
        slab->color_map = (uint64_t *)((uint8_t *)slab + sizeof(struct Slab));

        slab->free_count = (PAGE_2M_SIZE - (PAGE_2M_SIZE / size / 8) - sizeof(struct Slab)) / size;
        slab->using_count = 0;
        slab->color_count = slab->free_count;
        slab->address = (void *)page->phy_address;
        slab->page = page;
        list_init(&slab->list);

        slab->color_length = ((slab->color_count + sizeof(uint64_t) * 8 - 1) >> 6) << 3;
        memset(slab->color_map, 0xff, slab->color_length);

        for (i = 0; i < slab->color_count; i++)
            *(slab->color_map + (i >> 6)) ^= 1UL << i % 64;
        
        break;

    // kmalloc slab and map, not in 2M page anymore
    case 1024:
    case 2048:
    case 4096:
    case 8192:
    case 16384:
    // color_map is a very short buffer
    case 32768:
    case 65536:
    case 131072:
    case 262144:
    case 524288:
    case 1048576:

        slab = (struct Slab *)kmalloc(sizeof(struct Slab));

        slab->free_count = PAGE_2M_SIZE / size;
        slab->using_count = 0;
        slab->color_count = slab->free_count;

        slab->color_length = ((slab->color_count + sizeof(uint64_t) * 8 - 1) >> 6) << 3;

        slab->color_map = (uint64_t *)kmalloc(slab->color_length);
        memset(slab->color_map, 0xff, slab->color_length);

        slab->address = (void *)page->phy_address;
        slab->page = page;
        list_init(&slab->list);

        for (i = 0;i < slab->color_count; i++)
            *(slab->color_map + (i >> 6)) ^= 1UL << i % 64;
        
        break;

    default:
        color_printk(RED,BLACK,"kmalloc_create() ERROR: wrong size:%08d\n",size);
		free_pages(page,1);

        return NULL;
    }

    return slab;
}

void * kmalloc(size_t size)
{
    uint32_t i, j;
    struct Slab * slab = NULL;
    if (size > 1048576)
    {
        color_printk(RED,BLACK,"kmalloc() ERROR: kmalloc size too long:%08d\n",size);
		return NULL;
    }
    for (i = 0; i < 16; i++)
    {
        if (kmalloc_cache_size[i].size >= size)
            break;
    }
    slab = kmalloc_cache_size[i].cache_pool;

    if (kmalloc_cache_size[i].total_free != 0)
    {
        do
        {
            if (slab->free_count == 0)
                slab = container_of(list_next(&slab->list), struct Slab, list);
            else
                break;
        } while (slab != kmalloc_cache_size[i].cache_pool);
        
    }
    else
    {
        slab = kmalloc_create(kmalloc_cache_size[i].size);
        if (slab == NULL)
        {
            color_printk(RED, BLACK, "kmalloc()->kmalloc_create()=>slab == NULL\n");
            return NULL;
        }
        
        kmalloc_cache_size[i].total_free += slab->color_count;
        color_printk(BLUE,BLACK,"kmalloc()->kmalloc_create()<=size:%#010x\n",kmalloc_cache_size[i].size);
		list_add_to_before(&kmalloc_cache_size[i].cache_pool->list,&slab->list);
	}
    
    for (j = 0; j < slab->color_count; j++)
    {
        if (*(slab->color_map + (j >> 6)) == 0xffffffffffffffffUL)
        {
            j += 63;
            continue;
        }
        
        if ((*(slab->color_map + (j >> 6)) & (1UL << (j % 64))) == 0)
        {
            *(slab->color_map + (j >> 6)) |= 1UL << (j % 64);
            slab->using_count++;
            slab->free_count--;

            kmalloc_cache_size[i].total_free--;
            kmalloc_cache_size[i].total_using++;

            return (void *)((char *)slab->address + kmalloc_cache_size[i].size * j);
        }
    }

	color_printk(BLUE,BLACK,"kmalloc() ERROR: no memory can alloc\n");
	return NULL;
}

size_t kfree(void * address)
{
    uint32_t i, index;
    struct Slab * slab = NULL;
    void * page_base_address = (void *)((uint64_t)address & PAGE_2M_MASK);

    for (i = 0;i < 16; i++)
    {
        slab = kmalloc_cache_size[i].cache_pool;
        do
        {
            if (slab->address == page_base_address)
            {
                index = (address - slab->address) / kmalloc_cache_size[i].size;

                *(slab->color_map + (index >> 6)) ^= 1UL << index % 64;

                slab->free_count++;
                slab->using_count--;

                kmalloc_cache_size[i].total_free++;
                kmalloc_cache_size[i].total_using--;

                // free current slab when it was expanded and here have enough space in slab pool (1.5 of current slab size)
                if ((slab->using_count==0) && (kmalloc_cache_size[i].total_free >= slab->color_count * 3 / 2) && kmalloc_cache_size[i].cache_pool != slab)
                {
                    switch (kmalloc_cache_size[i].size)
                    {
                    case 32:
                    case 64:
                    case 128:
                    case 256:
                    case 512:
                        list_del(&slab->list);
                        kmalloc_cache_size[i].total_free -= slab->color_count;

                        page_clean(slab->page);
                        free_pages(slab->page, 1);
                        break;
                    
                    default:
                        list_del(&slab->list);
                        kmalloc_cache_size[i].total_free -= slab->color_count;

                        kfree(slab->color_map);
                        page_clean(slab->page);
                        free_pages(slab->page, 1);
                        kfree(slab);
                        break;
                    }
                }
                
                return 1;
            }
            else
                slab = container_of(list_next(&slab->list), struct Slab, list);
            
        } while (slab != kmalloc_cache_size[i].cache_pool);
    }

    color_printk(RED, BLACK, "kfree() ERROR: can not free memory %p\n", address);

    return 0;
}

size_t slab_init()
{
    struct Page * page = NULL;
    uint64_t * physical = NULL;
    uint64_t i, j;

    uint64_t tmp_address = PMMngr.end_of_struct;

    for (i = 0; i < 16; i++)
    {
        kmalloc_cache_size[i].cache_pool = (struct Slab *)PMMngr.end_of_struct;
        PMMngr.end_of_struct = PMMngr.end_of_struct + sizeof(struct Slab) + sizeof(long) * 10;

        list_init(&kmalloc_cache_size[i].cache_pool->list);

        kmalloc_cache_size[i].cache_pool->using_count = 0;
        kmalloc_cache_size[i].cache_pool->free_count = PAGE_2M_SIZE / kmalloc_cache_size[i].size;
        kmalloc_cache_size[i].cache_pool->color_length = ((PAGE_2M_SIZE / kmalloc_cache_size[i].size + sizeof(uint64_t) * 8 - 1) >> 6) << 3;
        kmalloc_cache_size[i].cache_pool->color_count = kmalloc_cache_size[i].cache_pool->free_count;
        kmalloc_cache_size[i].cache_pool->color_map = (uint64_t *)PMMngr.end_of_struct;
        PMMngr.end_of_struct = (uint64_t)(PMMngr.end_of_struct + kmalloc_cache_size[i].cache_pool->color_length + sizeof(long) * 10) & ( ~ (sizeof(long) - 1));
        memset(kmalloc_cache_size[i].cache_pool->color_map, 0xff, kmalloc_cache_size[i].cache_pool->color_length);

        for (j = 0; j < kmalloc_cache_size[i].cache_pool->color_count; j++)
            *(kmalloc_cache_size[i].cache_pool->color_map + (j >> 6)) ^= 1UL << j % 64;
        
        kmalloc_cache_size[i].total_free = kmalloc_cache_size[i].cache_pool->color_count;
        kmalloc_cache_size[i].total_using = 0;
    }
    
    i = PMMngr.end_of_struct >> PAGE_2M_SHIFT;
    for (j = PAGE_2M_ALIGN(tmp_address) >> PAGE_2M_SHIFT; j <= i; j++)
    {
        page = PMMngr.pages_struct + j;
        *(PMMngr.bits_map + ((page->phy_address >> PAGE_2M_SHIFT) >> 6)) |= 1UL << (page->phy_address >> PAGE_2M_SHIFT) % 64;
        page->zone_struct->page_using_count++;
        page->zone_struct->page_free_count--;
        page_init(page, PG_PTable_Mapped | PG_Kernel_Init | PG_Kernel);
    }

    color_printk(ORANGE,BLACK,"2.PMMngr.bits_map:%#018lx\tzone_struct->page_using_count:%d\tzone_struct->page_free_count:%d\n",*PMMngr.bits_map,PMMngr.zones_struct->page_using_count,PMMngr.zones_struct->page_free_count);

    for(i = 0;i < 16;i++)
	{
		physical = (unsigned long *)((PMMngr.end_of_struct + PAGE_2M_SIZE * i + PAGE_2M_SIZE - 1) & PAGE_2M_MASK);
		page = Phy_to_2M_Page(physical);

		*(PMMngr.bits_map + ((page->phy_address >> PAGE_2M_SHIFT) >> 6)) |= 1UL << (page->phy_address >> PAGE_2M_SHIFT) % 64;
		page->zone_struct->page_using_count++;
		page->zone_struct->page_free_count--;

		page_init(page,PG_PTable_Mapped | PG_Kernel_Init | PG_Kernel);

		kmalloc_cache_size[i].cache_pool->page = page;
		kmalloc_cache_size[i].cache_pool->address = physical;
	}

	color_printk(ORANGE,BLACK,"3.PMMngr.bits_map:%#018lx\tzone_struct->page_using_count:%d\tzone_struct->page_free_count:%d\n",*PMMngr.bits_map,PMMngr.zones_struct->page_using_count,PMMngr.zones_struct->page_free_count);

	color_printk(ORANGE,BLACK,"start_code:%#018lx,end_code:%#018lx,end_data:%#018lx,end_brk:%#018lx,end_of_struct:%#018lx\n",PMMngr.start_code,PMMngr.end_code,PMMngr.end_data,PMMngr.start_brk, PMMngr.end_of_struct);

	return 1;
}