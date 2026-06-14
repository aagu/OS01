OUTPUT_FORMAT("elf64-x86-64","elf64-x86-64","elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(trampoline_code_start)

SECTIONS
{
    . = 0x8000;

    .text : {
        *(.text)
    }

    . = ALIGN(16);

    .rodata : {
        *(.rodata)
    }

    . = ALIGN(64);

    .data : {
        *(.data)
    }

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
    }
}
