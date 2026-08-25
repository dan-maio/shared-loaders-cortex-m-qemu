/* Section placement for the no-pic XIP/static app — board build.
 *
 * The firmware reserves two FIXED RAM windows for the loaded modules:
 *   _sxip_text_* = 0x20014000..0x2001C000  (32K, module R-X segments)
 *   _sxip_data_* = 0x2001C000..0x2001E000  ( 8K, module RW segments)
 * The fixtures are LINKED at exactly these VAs (see the fixtures' nopic-split.ld),
 * so the loader copies them with a 0 bias and the module's baked absolute
 * addresses are correct. bss + image_arena must stay below the text window
 * (asserted below); the heap lives between the arena and the text window.
 */
MEMORY
{
  FLASH    (rx)  : ORIGIN = 0x08000000, LENGTH = 256K
  FLASH_FS (r)   : ORIGIN = 0x08040000, LENGTH = 256K
  RAM      (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
__stack = _estack;

_fs_base = ORIGIN(FLASH_FS);
_fs_size = LENGTH(FLASH_FS);
_simage_arena_size = 16K;

/* no-pic fixed windows (module linked at these exact VAs — do not move) */
_sxip_text_start = 0x20014000;
_sxip_text_end   = 0x2001C000;
_sxip_data_start = 0x2001C000;
_sxip_data_end   = 0x2001E000;

__Main_Stack_Size = 1024 * 3 ;

ENTRY(_start)

SECTIONS
{
  .isr_vector : ALIGN(4)
  {
    __vectors_start = ABSOLUTE(.) ;
    __vectors_start__ = ABSOLUTE(.) ;
    KEEP(*(.isr_vector))
    *(.after_vectors .after_vectors.*)
  } >FLASH

  .text : ALIGN(4)
  {
    *(.text .text.*)
    *(.glue_7) *(.glue_7t)
    KEEP(*(.init)) KEEP(*(.fini))
  } >FLASH

  .rodata : ALIGN(4)
  {
    *(.rodata .rodata.*)
  } >FLASH

  .ARM.exidx : ALIGN(4)
  {
    __exidx_start = .;
    *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    __exidx_end = .;
  } >FLASH

  .rel.dyn : ALIGN(4)
  {
    *(.rel.dyn)
  } >FLASH
  __rel_dyn_start = ADDR(.rel.dyn);
  __rel_dyn_end   = ADDR(.rel.dyn) + SIZEOF(.rel.dyn);

  .dynsym : ALIGN(4)
  {
    *(.dynsym*)
  } >FLASH
  __dynsym_start = ADDR(.dynsym);
  __dynsym_end   = ADDR(.dynsym) + SIZEOF(.dynsym);

  .dynstr : ALIGN(4)
  {
    *(.dynstr*)
  } >FLASH
  __dynstr_start = ADDR(.dynstr);
  __dynstr_end   = ADDR(.dynstr) + SIZEOF(.dynstr);

  .hash : ALIGN(4)
  {
    *(.hash*)
  } >FLASH

  /* Constructor/Destructor arrays for C++ hosted startup */
  .inits : ALIGN(4)
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP(*(.preinit_array .preinit_array.*))
    PROVIDE_HIDDEN (__preinit_array_end = .);

    . = ALIGN(4);
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array))
    PROVIDE_HIDDEN (__init_array_end = .);

    . = ALIGN(4);
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP(*(SORT(.fini_array.*)))
    KEEP(*(.fini_array))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } >FLASH

  . = ALIGN(4);
  _sidata = LOADADDR(.data);
  .data : ALIGN(4)
  {
    _sdata = .;
    *(.data_begin .data_begin.*)
    *(.data .data.*)
    *(.ramfunc .ramfunc.*)
    *(.dynamic)

    /* Dummy symbols to bypass the buggy vendor assembly copy loop */
    _sgot = .;
    _esgot = .;
    _sgotdata = .;

    _sgot_plt = .;
    _esgot_plt = .;
    _sgotpltdata = .;

    /* Real GOT and GOT.PLT sections, copied to RAM by startup.c */
    *(.got)
    *(.got.plt)

    *(.data_end .data_end.*)
    . = ALIGN(4);
    _edata = .;
  } >RAM AT>FLASH

  .bss (NOLOAD) : ALIGN(4)
  {
    __bss_start__ = .;
    _sbss = .;
    *(.bss_begin .bss_begin.*)
    *(.bss .bss.*)
    *(COMMON)
    *(.bss_end .bss_end.*)
    . = ALIGN(4);
    __bss_end__ = .;
    _ebss = .;
  } >RAM

  .image_arena (NOLOAD) : ALIGN(8)
  {
    _simage_arena_start = .;
    . = . + _simage_arena_size;
    _simage_arena_end = .;
  } >RAM

  _end_noinit = _simage_arena_end;
  PROVIDE ( _Heap_Begin = _end_noinit ) ;
  PROVIDE ( _Heap_Limit = _sxip_text_start - 4 ) ;

  /DISCARD/ : { *(.ARM.attributes) *(.comment) }
}

ASSERT(_simage_arena_end <= _sxip_text_start,
       "no-pic: bss + image_arena overflow into the module text window")
ASSERT(_sidata + (_edata - _sdata) <= _fs_base,
       "flat_fs: code + init image overflowed the 256K code region into FLASH_FS")
