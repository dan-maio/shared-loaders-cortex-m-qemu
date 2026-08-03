/* Linker script for libcppsh.so — Cortex-M4 shared object.
 * Base address 0 to avoid base_va offset issues in the loader.
 * Single LOAD segment (RWX) for simplicity. */
OUTPUT_FORMAT("elf32-littlearm", "elf32-bigarm", "elf32-littlearm")
OUTPUT_ARCH(arm)

SECTIONS
{
  . = SEGMENT_START("text-segment", 0) + SIZEOF_HEADERS;

  /* Hash table for symbol lookup */
  .hash           : { *(.hash) }
  .gnu.hash       : { *(.gnu.hash) }

  /* Dynamic symbol table */
  .dynsym         : { *(.dynsym) }
  .dynstr         : { *(.dynstr) }

  /* Versioning */
  .gnu.version    : { *(.gnu.version) }
  .gnu.version_r  : { *(.gnu.version_r) }

  /* Relocation sections */
  .rel.dyn        : { *(.rel.init) *(.rel.text .rel.text.*) *(.rel.fini)
                      *(.rel.data .rel.data.*) *(.rel.rodata .rel.rodata.*)
                      *(.rel.data.rel.ro .rel.data.rel.ro.*)
                      *(.rel.got) *(.rel.bss) *(.rel.plt) }
  .rel.plt        : { *(.rel.plt) }

  /* PLT stubs for external function calls */
  .plt            : { *(.plt) *(.iplt) }
  .iplt           : { *(.iplt) }

  /* Code */
  .text           : { *(.text.unlikely .text.*_unlikely .text.unlikely.*)
                      *(.text.exit .text.exit.*)
                      *(.text.startup .text.startup.*)
                      *(.text.hot .text.hot.*)
                      *(.text .text.*) *(.gnu.linkonce.t.*)
                      *(.glue_7t) *(.glue_7) *(.vfp11_veneer)
                      *(.ARM.extab .ARM.extab.*)
                      *(.gcc_except_table .gcc_except_table.*) }

  /* Read-only data */
  .rodata         : { *(.rodata .rodata.* .gnu.linkonce.r.*) }
  .rodata1        : { *(.rodata1) }

  /* Constructors and destructors */
  .preinit_array  : { KEEP(*(.preinit_array)) }
  .init_array     : { KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) .init_array)) }
  .fini_array     : { KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) .fini_array)) }

  /* Dynamic linking info */
  .dynamic        : { *(.dynamic) }

  /* Global Offset Table */
  .got            : { *(.got) }
  .got.plt        : { *(.got.plt) }

  /* Data sections */
  .data.rel.ro    : { *(.data.rel.ro.local .data.rel.ro .data.rel.ro.*) }
  .data           : { *(.data .data.* .gnu.linkonce.d.*) }
  .data1          : { *(.data1) }
  .bss            : { *(.bss .bss.* .gnu.linkonce.b.*) *(COMMON) }
  .noinit         : { *(.noinit .noinit.*) }

  /* Debug sections (stripped by --strip-debug, kept by -g) */
  .debug_aranges   : { *(.debug_aranges) }
  .debug_info      : { *(.debug_info) }
  .debug_abbrev    : { *(.debug_abbrev) }
  .debug_line      : { *(.debug_line) }
  .debug_frame     : { *(.debug_frame) }
  .debug_str       : { *(.debug_str) }
  .debug_loc       : { *(.debug_loc) }
  .debug_macinfo   : { *(.debug_macinfo) }
  .debug_ranges    : { *(.debug_ranges) }
  .debug_macro     : { *(.debug_macro) }

  /* ARM attributes */
  .ARM.attributes  : { *(.ARM.attributes) }
}
