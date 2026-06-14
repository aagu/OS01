// ──────────────────────────────────────────────────────────
//  AP Startup Trampoline
//
//  This code is assembled and linked separately at 0x8000
//  (see trampoline.ld), then the binary is embedded in the
//  kernel via objcopy → ld -r -b binary.
//
//  At runtime, the BSP copies the embedded blob to physical
//  address 0x8000, fills trampoline_data, and sends
//  INIT-SIPI-SIPI to each AP.
//
//  All label references here resolve to 0x8xxx — well within
//  32-bit range — so 16/32-bit code can use absolute addressing.
// ──────────────────────────────────────────────────────────

.code16
.globl trampoline_code_start
trampoline_code_start:

    cli
    cld

    // Disable legacy 8259 PIC
    movb    $0xFF, %al
    outb    %al, $0xA1
    outb    %al, $0x21

    // Load trampoline GDT
    lgdtl   trampoline_gdt_ptr

    // Enter protected mode
    movl    %cr0, %eax
    orl     $1, %eax
    movl    %eax, %cr0

    ljmpl   $0x08, $trampoline_protected

.code32
trampoline_protected:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %fs
    movw    %ax, %gs
    movw    %ax, %ss

    // Enable PAE
    movl    %cr4, %eax
    orl     $(1 << 5), %eax
    movl    %eax, %cr4

    // Load PML4 from trampoline data
    movl    $trampoline_data, %esi
    movl    0(%esi), %eax                  // tdata->cr3 (low 32 bits)
    movl    %eax, %cr3

    // Enable Long Mode
    movl    $0xC0000080, %ecx
    rdmsr
    orl     $(1 << 8), %eax                // LME
    wrmsr

    // Enable paging
    movl    %cr0, %eax
    orl     $(1 << 31), %eax
    movl    %eax, %cr0

    ljmpl   $0x18, $trampoline_long

.code64
trampoline_long:
    movw    $0x20, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss

    // Load per-CPU GS base via MSR
    movq    trampoline_data + 8(%rip), %rax // tdata->gs_base
    movl    $0xC0000101, %ecx               // IA32_GS_BASE
    wrmsr

    // Load kernel stack
    movq    trampoline_data + 16(%rip), %rsp // tdata->stack

    // Call ap_entry
    movq    trampoline_data + 24(%rip), %rax // tdata->entry
    call    *%rax

    // Not reached
1:  hlt
    jmp     1b


// ──────────────────────────────────────────────────────────
//  Trampoline GDT (physical base = 0x8000 + offset)
// ──────────────────────────────────────────────────────────

.align 16
trampoline_gdt:
    .quad   0x0000000000000000              // 0x00: NULL
    .quad   0x00CF9B000000FFFF              // 0x08: 32-bit CS
    .quad   0x00CF93000000FFFF              // 0x10: 32-bit DS
    .quad   0x00209B0000000000              // 0x18: 64-bit CS
    .quad   0x0020930000000000              // 0x20: 64-bit DS
trampoline_gdt_end:

trampoline_gdt_ptr:
    .word   trampoline_gdt_end - trampoline_gdt - 1
    .long   trampoline_gdt


// ──────────────────────────────────────────────────────────
//  Trampoline data — at fixed offset 0x200 from the binary
//  start (= physical address 0x8200).
//  BSP fills via Phy_To_Virt(0x8200).
//  Keep layout in sync with trampoline_data_t in trampoline.h.
// ──────────────────────────────────────────────────────────

// Pad to offset 0x200 from section base (= physical 0x8200 after linking)
.org    0x200

.globl trampoline_data
trampoline_data:
    .quad   0   //  0: cr3
    .quad   0   //  8: gs_base
    .quad   0   // 16: stack
    .quad   0   // 24: entry
    .long   0   // 32: apic_id
    .long   0   // 36: cpu_id

.globl trampoline_code_end
trampoline_code_end:
