/* Section placement for the PIC boot base.
 * Code/const -> FLASH.  .data/.got/.got.plt -> RAM with a FLASH load image (LMA).
 * Symbols consumed by startup.c's Reset_Handler.
 */
INCLUDE mem.ld          /* found via the linker -Lfirmware/ldscripts search path */
ENTRY(Reset_Handler)

SECTIONS
{
  .isr_vector : ALIGN(4)
  {
    KEEP(*(.isr_vector))
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

  /* One contiguous RAM image (.data, .dynamic, .got, .got.plt) sharing a single
   * FLASH load image (LMA). Reset_Handler copies the whole [_sdata, _edata) block
   * in one linear pass, so every section in between is initialized.
   *
   * .dynamic MUST live inside this copied block: link_context::from_self() reads
   * _DYNAMIC at its RAM VMA to resolve loaded-library imports against main. The
   * linker would otherwise drop this orphan section between .data and .got, where
   * the previous per-section copy loops skipped it -> _DYNAMIC was uninitialized
   * garbage and dlopen failed with BadSection. */
  . = ALIGN(4);
  _sidata = LOADADDR(.data);
  .data : ALIGN(4)
  {
    _sdata = .;
    *(.data .data.*)
  } >RAM AT>FLASH

  .dynamic : ALIGN(4)
  {
    *(.dynamic)
  } >RAM AT>FLASH

  .got : ALIGN(4)
  {
    _sgot = .;
    *(.got)
  } >RAM AT>FLASH

  .got.plt : ALIGN(4)
  {
    _sgot_plt = .;
    *(.got.plt)
    . = ALIGN(4);
    _edata = .;          /* end of the whole contiguous init-data image */
  } >RAM AT>FLASH

  .bss (NOLOAD) : ALIGN(4)
  {
    _sbss = .;
    *(.bss .bss.*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } >RAM

  .image_arena (NOLOAD) : ALIGN(8)
  {
    _simage_arena_start = .;
    . = . + _simage_arena_size;
    _simage_arena_end = .;
  } >RAM

  /* Keep .debug_* in the ELF for source-level debugging (GDB). They are not
   * allocated, so they are never flashed and objcopy excludes them from .bin/.hex. */
  /DISCARD/ : { *(.ARM.attributes) *(.comment) }
}
