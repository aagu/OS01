#ifndef _KERNEL_VMM_H
#define _KERNEL_VMM_H

#include <stdint.h>

#define KERNEL_BASE 0x100000

//page table attribute

//bit 63 Execution Disable:
#define PAGE_XD       (1UL << 63)
//bit 12 Page Attribute Table:
#define PAGE_PAT      (1UL << 12)
//bit 8 Global Page:1,global;0,part
#define PAGE_Global   (1UL << 8)
//bit 7 Page Size:1,big page;0,small page
#define PAGE_PS       (1UL << 7)
//bit 6 Dirty:1,dirty;0,clean
#define PAGE_Dirty    (1UL << 6)
//bit 5 Accessed:1,visited;0,unvisited
#define PAGE_Accessed (1UL << 5)
//bit 4 Page Level Cache Disable
#define PAGE_PCD      (1UL << 4)
//bit 3 Page Level Write Through
#define PAGE_PWT      (1UL << 3)
//bit 2 User Supervisor:1,user and supervisor;0,supervisor
#define PAGE_U_S      (1UL << 2)
//bit 1 Read Write:1,read and write;0,read
#define PAGE_R_W      (1UL << 1)
//bit 0 Present:1,present;0,not present
#define PAGE_Present  (1UL << 0)

#define PAGE_KERNEL_GDT  (PAGE_R_W | PAGE_Present)
#define PAGE_KERNEL_Dir  (PAGE_R_W | PAGE_Present)
#define	PAGE_KERNEL_Page (PAGE_PS  | PAGE_R_W | PAGE_Present)
#define PAGE_USER_GDT    (PAGE_U_S | PAGE_R_W | PAGE_Present)
#define PAGE_USER_Dir    (PAGE_U_S | PAGE_R_W | PAGE_Present)
#define	PAGE_USER_Page   (PAGE_PS  | PAGE_U_S | PAGE_R_W | PAGE_Present)

typedef struct {uint64_t pte;} pte_t;
#define mk_pt(addr,attr)	((unsigned long)(addr) | (unsigned long)(attr))
#define set_pt(ptptr,ptval)		(*(ptptr) = (ptval))

typedef struct {uint64_t pde;} pde_t;
#define mk_pdt(addr,attr)	((unsigned long)(addr) | (unsigned long)(attr))
#define set_pdt(pdtptr,pdtval)		(*(pdtptr) = (pdtval))

typedef struct {uint64_t pdpte;} pdpte_t;
#define mk_pdpt(addr,attr)	((unsigned long)(addr) | (unsigned long)(attr))
#define set_pdpt(pdptptr,pdptval)	(*(pdptptr) = (pdptval))

typedef struct {uint64_t pml4e;} pml4e_t;
#define mk_pml4t(addr,attr) ((unsigned long)(addr) | (unsigned long)(attr))
#define set_pml4t(pml4tptr,pml4tval) (*(pml4tptr) = (pml4tval))

#define VM_PML4T (KERNEL_BASE + 0x1000)
#define VM_PDPT  (KERNEL_BASE + 0x2000)
#define VM_PDT   (KERNEL_BASE + 0x3000)

#endif