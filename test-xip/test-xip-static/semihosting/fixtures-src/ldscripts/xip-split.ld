/*
 * xip-split.ld — Linker script to generate split R-X | RW segments for XIP execution.
 *
 * Text and data segments have non-overlapping VA ranges so the PLT's
 * PC-relative GOT calculation reaches the correct flash-resident GOT entry.
 * The condition data_vaddr - data_off == text_vaddr - text_off ensures this;
 * we set . = SIZEOF_HEADERS so text_vaddr == text_off (offset cancelled).
 */
OUTPUT_FORMAT("elf32-littlearm", "elf32-bigarm", "elf32-littlearm")
OUTPUT_ARCH(arm)

PHDRS
{
  text PT_LOAD FLAGS(5);   /* R-X: stays in flash */
  data PT_LOAD FLAGS(6);   /* RW: copied to RAM */
}

SECTIONS
{
  . = SIZEOF_HEADERS;

  /* ===== R-X Segment (stays in Flash) ===== */
  .hash           : { *(.hash) } :text
  .gnu.hash       : { *(.gnu.hash) } :text
  .dynsym         : { *(.dynsym) } :text
  .dynstr         : { *(.dynstr) } :text
  .rel.dyn        : { *(.rel*) } :text
  .text           : { *(.text .text.*) } :text
  .rodata         : { *(.rodata .rodata.*) } :text
  .plt            : { *(.plt) } :text
  .ARM.extab      : { *(.ARM.extab*) } :text
  .ARM.exidx      : { *(.ARM.exidx*) } :text

  . = ALIGN(16);

  /* ===== RW Segment (copied to RAM); data_vaddr == data_off automatically ===== */
  .preinit_array  : { KEEP(*(.preinit_array)) } :data
  .init_array     : { KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array)) } :data
  .fini_array     : { KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array)) } :data
  .data.rel.ro    : { *(.data.rel.ro .data.rel.ro.*) } :data
  .dynamic        : { *(.dynamic) } :data
  .got            : { *(.got*) } :data
  .data           : { *(.data .data.*) } :data
  .bss            : { *(.bss .bss.*) *(COMMON) } :data
}
