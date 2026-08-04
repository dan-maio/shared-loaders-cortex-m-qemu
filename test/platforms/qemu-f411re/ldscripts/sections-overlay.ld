/* Section placement for the OVERLAY firmware.
 *
 * Same PIC boot base as sections.ld (code/const -> FLASH; .data/.got/.got.plt ->
 * RAM with a FLASH load image copied by Reset_Handler), PLUS one OVERLAY block:
 * three modules that share a single SRAM VMA window and are stored back-to-back
 * in FLASH. The linker binds every cross-reference (main<->overlay, overlay->main)
 * at LINK TIME to fixed addresses; the overlay manager only memcpy()s the chosen
 * module's bytes from its FLASH load address into the shared window before use.
 */
INCLUDE mem-overlay.ld   /* found via the linker -Lfirmware/ldscripts search path */
ENTRY(Reset_Handler)

SECTIONS
{
  .isr_vector : ALIGN(4) { KEEP(*(.isr_vector)) } >FLASH

  .text : ALIGN(4)
  {
    *(.text .text.*)
    *(.glue_7) *(.glue_7t)
    KEEP(*(.init)) KEEP(*(.fini))
  } >FLASH

  .rodata : ALIGN(4) { *(.rodata .rodata.*) } >FLASH

  .ARM.exidx : ALIGN(4)
  {
    __exidx_start = .;
    *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    __exidx_end = .;
  } >FLASH

  .rel.dyn : ALIGN(4) { *(.rel.dyn) } >FLASH

  /* Contiguous RAM init image (.data/.dynamic/.got/.got.plt) with one FLASH LMA,
   * copied in one pass by Reset_Handler. Identical to sections.ld -- see there. */
  . = ALIGN(4);
  _sidata = LOADADDR(.data);
  .data : ALIGN(4) { _sdata = .; *(.data .data.*) } >RAM AT>FLASH
  .dynamic : ALIGN(4) { *(.dynamic) } >RAM AT>FLASH
  .got : ALIGN(4) { _sgot = .; *(.got) } >RAM AT>FLASH
  .got.plt : ALIGN(4)
  {
    _sgot_plt = .;
    *(.got.plt)
    . = ALIGN(4);
    _edata = .;                /* end of the contiguous init-data image */
  } >RAM AT>FLASH

  /* ===================== THE OVERLAY ===================== */
  /* Three modules, ONE shared VMA (the OVL region @ 0x20018000), stored back-to-back
   * in FLASH (AT>FLASH continues the load counter after the init image above). Every
   * .ovN.text/.rodata/.data is gathered per module; KEEP() defeats --gc-sections (main
   * reaches them by symbol, but the section names are non-standard). NOCROSSREFS makes
   * ld FAIL the link if one overlay ever references another -- they can never be
   * co-resident, so such a reference would be a latent HardFault. */
  OVERLAY : NOCROSSREFS
  {
    .ov1   { KEEP(*(.ov1.text))   KEEP(*(.ov1.rodata))   KEEP(*(.ov1.data)) }
    .ov2   { KEEP(*(.ov2.text))   KEEP(*(.ov2.rodata))   KEEP(*(.ov2.data)) }
    .ovcpp { KEEP(*(.ovcpp.text)) KEEP(*(.ovcpp.rodata)) KEEP(*(.ovcpp.data)) }
  } >OVL AT>FLASH

  /* Robust load-image symbols for the overlay manager: FLASH load address (LMA) and
   * byte size of each module. (GNU ld also auto-defines __load_start_ovN/__load_stop_ovN,
   * but we derive ours from LOADADDR/SIZEOF to avoid any name-mangling ambiguity.) */
  _ov1_lma   = LOADADDR(.ov1);    _ov1_size   = SIZEOF(.ov1);
  _ov2_lma   = LOADADDR(.ov2);    _ov2_size   = SIZEOF(.ov2);
  _ovcpp_lma = LOADADDR(.ovcpp);  _ovcpp_size = SIZEOF(.ovcpp);
  _ovl_vma   = ADDR(.ov1);        /* shared run address == ORIGIN(OVL) == _ovl_window */
  /* ======================================================= */

  .bss (NOLOAD) : ALIGN(4)
  {
    _sbss = .;
    *(.bss .bss.*) *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } >RAM

  .image_arena (NOLOAD) : ALIGN(8)
  {
    _simage_arena_start = .;
    . = . + _simage_arena_size;
    _simage_arena_end = .;
  } >RAM

  /DISCARD/ : { *(.ARM.attributes) *(.comment) }
}

/* Each overlay module must fit the shared window (ld also errors on OVL overflow, but
 * this names the offender). */
ASSERT(SIZEOF(.ov1)   <= _ovl_window_size, "overlay ov1 larger than the 16K OVL window")
ASSERT(SIZEOF(.ov2)   <= _ovl_window_size, "overlay ov2 larger than the 16K OVL window")
ASSERT(SIZEOF(.ovcpp) <= _ovl_window_size, "overlay ovcpp larger than the 16K OVL window")
