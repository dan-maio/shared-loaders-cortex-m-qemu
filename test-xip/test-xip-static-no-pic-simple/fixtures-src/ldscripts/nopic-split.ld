/*
 * nopic-split.ld — Linker script for the no-pic XIP/static fixture modules.
 *
 * The module is built `-fno-pic -mlong-calls` and linked at FIXED addresses, so
 * the produced code is position-dependent: every intra-module reference is an
 * absolute address baked in at link time (R_ARM_RELATIVE addends are absolute).
 * That is the whole trick — there is no GOT, no PLT, no PIC base register (r10),
 * and main can call the exports with a plain function pointer.
 *
 * The two VAs are filled in by build.sh (sed) so each fixture lives in its own
 * sub-slot of the firmware's reserved windows:
 *
 *   text VA = __TEXT_BASE__   (R-X segment -> copied there at load)
 *   data VA = __DATA_BASE__   (RW segment  -> copied there at load)
 *
 * Every address must fall inside the app linker script's `_sxip_text_*` /
 * `_sxip_data_*` ranges (0x20018000..0x2001C000 / 0x2001C000..0x2001D000) and
 * no two modules loaded at the same time may share a VA. The loader copies with
 * a 0 bias, so the linker-time addresses ARE the runtime addresses.
 */
OUTPUT_FORMAT("elf32-littlearm", "elf32-bigarm", "elf32-littlearm")
OUTPUT_ARCH(arm)

PHDRS
{
  text PT_LOAD FLAGS(5);   /* R-X (in RAM at runtime, but immutable) */
  data PT_LOAD FLAGS(6);   /* RW: copied to the data window          */
}

SECTIONS
{
  /* ===== R-X segment — executes from __TEXT_BASE__ ===== */
  . = __TEXT_BASE__;
  .hash           : { *(.hash) } :text
  .dynsym         : { *(.dynsym) } :text
  .dynstr         : { *(.dynstr) } :text
  .rel.dyn        : { *(.rel*) } :text
  .text           : { *(.text .text.*) } :text
  .rodata         : { *(.rodata .rodata.*) } :text
  .ARM.exidx      : { *(.ARM.exidx*) } :text
  .ARM.extab      : { *(.ARM.extab*) } :text

  /* ===== RW segment — copied to __DATA_BASE__, bss zeroed ===== */
  . = __DATA_BASE__;
  .init_array     : { KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array)) } :data
  .fini_array     : { KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array)) } :data
  .data.rel.ro    : { *(.data.rel.ro .data.rel.ro.*) } :data
  .dynamic        : { *(.dynamic) } :data
  .got            : { *(.got*) } :data
  .data           : { *(.data .data.*) } :data
  .bss            : { *(.bss .bss.*) *(COMMON) } :data
}
