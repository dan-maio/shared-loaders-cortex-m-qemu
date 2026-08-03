/* reloc.ld — a plain, non-PIC STM32F411RE firmware layout for the scl::reloc
 * test app. Deliberately simple: NONE of the ET_DYN dynamic-linking apparatus
 * (.dynsym/.dynstr/.hash/.rel.dyn, GOT-in-RAM, _DYNAMIC, the _reloc table) is
 * here, because this main is an ordinary non-PIC firmware — it resolves a loaded
 * module's imports through an explicit C++ export table, not through its own
 * .dynsym, and the modules run at absolute arena addresses.
 *
 * Compare test/platforms/weact-f411ce/ldscripts/sections.ld (the -pie script
 * the dlopen loaders need). This is the whole story for eager ET_REL loading.
 */
ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
    .isr_vector : ALIGN(4) { KEEP(*(.isr_vector)) } > FLASH

    .text : ALIGN(4)
    {
        KEEP(*(.text .text.*))
        KEEP(*(.rodata .rodata.* .constdata .constdata.*))
        *(.glue_7) *(.glue_7t)
        KEEP(*(.eh_frame*))
        . = ALIGN(4);
    } > FLASH

    .ARM.exidx : ALIGN(4) { *(.ARM.exidx* .gnu.linkonce.armexidx.*) } > FLASH

    .init_array : ALIGN(4)
    {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN(__init_array_end = .);
    } > FLASH

    . = ALIGN(4);
    _sidata = LOADADDR(.data);           /* FLASH master copy of .data */

    .data : ALIGN(4)
    {
        _sdata = .;
        KEEP(*(.data .data.*))
        . = ALIGN(4);
        _edata = .;
    } > RAM AT> FLASH

    .bss (NOLOAD) : ALIGN(4)
    {
        _sbss = .;
        KEEP(*(.bss .bss.*))
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    . = ALIGN(8);
    PROVIDE(end = .);                     /* not used (crt heap is a static array) */

    /DISCARD/ : { *(.ARM.attributes) *(.comment) }
}
