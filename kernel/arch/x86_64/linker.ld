OUTPUT_FORMAT("elf64-x86-64","elf64-x86-64","elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
	text PT_LOAD FLAGS(5);   /* PF_R | PF_X */
	data PT_LOAD FLAGS(6);   /* PF_R | PF_W */
}

/* Large-model sections must stay inside these bounds: PMM reuses
 * memory starting at _end, so orphan data there would be overwritten. */
SECTIONS
{

	. = 0xffff800000000000 + 0x100000;
	.text :
	{
		_text = .;
		*(.text.head)
		*(.text .text.*)
	} :text
	.ltext :
	{
		_ltext = .;
		*(.ltext .ltext.*)
		_eltext = .;
		_etext = .;
	} :text

	.rodata :
	{
		_rodata = .;
		*(.rodata .rodata.* .lrodata .lrodata.*)
	} :text

	/* PCI driver registration table. Drivers place static pointers here
	 * via PCI_DRIVER_DECLARE(); device_boot_init() iterates this range. */
	.pci_drivers :
	{
		. = ALIGN(8);
		PROVIDE(__pci_drivers_start = .);
		KEEP(*(.pci_drivers))
		PROVIDE(__pci_drivers_end = .);
	} :text

	_erodata = .;

	. = ALIGN(8);

	.data :
	{
		_data = .;
		/* Keep .data.init_task in its aligned output section below. */
		*(.data)
		*(.ldata .ldata.*)
		_edata = .;
	} :data

	/* Subsys self-registration table. Each driver places a function
	 * pointer here via SUBSYS_INITCALL(); arch_register_subsys()
	 * iterates this range to call each one. */
	.subsys_init :
	{
		PROVIDE(__subsys_init_start = .);
		KEEP(*(.subsys_init))
		PROVIDE(__subsys_init_end = .);
	} :data

	. = ALIGN(32768);
	.data.init_task : { *(.data.init_task) } :data

	.bss :
	{
		_bss = .;
		*(.bss .bss.* .lbss .lbss.*)
		*(COMMON)
		_ebss = .;
	} :data

	_end = .;
}
