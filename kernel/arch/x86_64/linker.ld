OUTPUT_FORMAT("elf64-x86-64","elf64-x86-64","elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
	text PT_LOAD FLAGS(5);   /* PF_R | PF_X */
	data PT_LOAD FLAGS(6);   /* PF_R | PF_W */
}

SECTIONS
{

	. = 0xffff800000000000 + 0x100000;
	.text :
	{
		_text = .;
		*(.text.head)
		*(.text)
		_etext = .;
	} :text
	.ltext :
	{
		_ltext = .;
		*(.ltext)
		_eltext = .;
	} :text

	.rodata :
	{
		_rodata = .;
		*(.rodata)
		_erodata = .;
	} :text

	. = ALIGN(8);

	.data :
	{
		_data = .;
		*(.data)
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
		*(.bss)
		_ebss = .;
	} :data

	_end = .;
}
