#include <stdint.h>
#include <stddef.h>
#include <kernel/memory.h>
#include <kernel/printk.h>

void pmm_init(struct MEMORY_INFO E820_Info)
{
    uint32_t i;
    uint64_t TotalMem = 0;
    struct E820_ENTRY *p = E820_Info.E820_Entry;

    color_printk(BLUE, BLACK, "Display Physics Address MAP,Type(1:RAM,2:ROM or Reserved,3:ACPI Reclaim Memory,4:ACPI NVS Memory,Others:Undefine)\n");
    for (i = 0; i < E820_Info.E820_Entry_count; i++)
    {
        // color_printk(ORANGE,BLACK,"MemoryMap (%10lx<->%10lx) %4d\n",p->address,p->address+p->length,p->type);
        color_printk(ORANGE,BLACK,"Address:%#018lx\tLength:%#018lx\tType:%2d\n",p->address,p->length,p->type);
		if(p->type == 1)
		{
			TotalMem += p->length;
		}

		p++;
    }

    color_printk(ORANGE,BLACK,"OS Can Used Total RAM:%dMB\n",TotalMem>>20);
}