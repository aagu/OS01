ENTRY(_start)
PHDRS {
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}
SECTIONS {
    .text 0x400000 : { *(.text) } :text
    .data 0x401000 : { *(.data) } :data
    .bss : { *(.bss) } :data
}
