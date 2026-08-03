SECTIONS
{
    .hash : { *(.hash) } > RAM
    .dynsym : { *(.dynsym) } > RAM
    .dynstr : { *(.dynstr) } > RAM
    .rel.dyn : { *(.rel.dyn) } > RAM
    .rodata : { *(.rodata .rodata.*) } > RAM
    .text : { *(.text .text.*) } > RAM
    .data : { *(.data .data.*) } > RAM
    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) } > RAM
    /DISCARD/ : { *(.ARM.attributes) *(.comment) *(.note.*) }
}
