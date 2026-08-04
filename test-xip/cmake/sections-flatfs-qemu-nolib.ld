/* Section placement for the hosted XIP flatfs boot base. */
MEMORY
{
  FLASH    (rx)  : ORIGIN = 0x08000000, LENGTH = 256K
  FLASH_FS (rw)  : ORIGIN = 0x10000000, LENGTH = 64K
  RAM      (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
__stack = _estack;

_fs_base = ORIGIN(FLASH_FS);
_fs_size = LENGTH(FLASH_FS);
_simage_arena_size = 64K;

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

  .dynsym : ALIGN(4)
  {
    *(.dynsym*)
  } >FLASH

  .dynstr : ALIGN(4)
  {
    *(.dynstr*)
  } >FLASH

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
  PROVIDE ( _Heap_Limit = __stack - __Main_Stack_Size ) ;

  /DISCARD/ : { *(.ARM.attributes) *(.comment) }
}

ASSERT(_sidata + (_edata - _sdata) <= _fs_base,
       "flat_fs: code + init image overflowed the 256K code region into FLASH_FS")
