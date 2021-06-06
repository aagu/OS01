#include <kernel/memory.h>
#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <stdint.h>

void vmm_init()
{
    uint64_t * Global_CR3 = get_cr3();
    color_printk(INDIGO,BLACK,"Global_CR3\t:%#018lx\n",Global_CR3);
    color_printk(INDIGO,BLACK,"*Global_CR3\t:%#018lx\n",*Global_CR3 & (~0xff));
	color_printk(PURPLE,BLACK,"**Global_CR3\t:%#018lx\n",*((uint64_t *)(*Global_CR3 & (~0xff))) & (~0xff));

    // int32_t i;
    // uint64_t * pml4t = (uint64_t *)VM_PML4T;
    // pml4t[0] = 0x102003;
    // for (i = 1; i < 256; i++)
    // {
    //     set_pml4t(pml4t+i, 0x0);
    // }
    // pml4t[256] = 0x102003;
    // for (i = 257; i < 512; i++)
    // {
    //     set_pml4t(pml4t+i, 0x0);
    // }
    // uint64_t * pdpt = (uint64_t *)VM_PDPT;
    // pdpt[0] = 0x103003;
    // for (i = 1; i < 512; i++)
    // {
    //     set_pdpt(pdpt+i, 0x0);
    // }
    // uint64_t * pdt = (uint64_t *)VM_PDT;
    // for (i = 0; i < 10; i++)
    // {
    //     set_pdt(pdt+i, mk_pdt((1UL << PAGE_2M_SHIFT) * i,PAGE_KERNEL_Page));
    // }
    // for (i = 11; i < 512; i++)
    // {
    //     set_pdt(pdt+i, 0x0);
    // }
    __asm__ __volatile__(
        "movq   %0, %%rax   \n\t"
        "movq   %%rax,  %%cr3   \n\t"
        :
        :"d"((uint64_t)VM_PML4T)
        :"memory"
    );
    Global_CR3 = get_cr3();
    color_printk(INDIGO,BLACK,"Global_CR3\t:%#018lx\n",Phy_To_Virt(Global_CR3));
}