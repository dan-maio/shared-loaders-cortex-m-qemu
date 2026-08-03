/* SCL overlay linker-script TEMPLATE — copy into the application's SECTIONS
 * and adapt the module list. NOT included automatically: the SCL never owns
 * linker scripts; the application does.
 *
 * Prerequisites in the application's MEMORY block: a dedicated region for the
 * shared window, e.g. (STM32F411RE reference layout):
 *
 *   MEMORY
 *   {
 *     FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
 *     RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 96K
 *     OVL   (rwx) : ORIGIN = 0x20018000, LENGTH = 16K   <-- the shared window
 *   }
 *
 * Module sources place their code/data in named sections, e.g.:
 *   __attribute__((section(".ov1.text"))) uint32_t ov1_entry(void) { ... }
 * Calls from flash-resident code into the window cross > Thumb BL reach:
 * declare window entry points __attribute__((long_call)).
 *
 * Inside the application's SECTIONS, after the .data/.got init image: */

  /* N modules, ONE shared VMA (the OVL region), stored back-to-back in FLASH
   * (AT>FLASH continues the load counter). KEEP() defeats --gc-sections (main
   * reaches the modules by symbol, but the section names are non-standard).
   * NOCROSSREFS makes ld FAIL the link if one overlay references another --
   * they are never co-resident, so such a reference is a latent HardFault. */
  OVERLAY : NOCROSSREFS
  {
    .ov1   { KEEP(*(.ov1.text))   KEEP(*(.ov1.rodata))   KEEP(*(.ov1.data)) }
    .ov2   { KEEP(*(.ov2.text))   KEEP(*(.ov2.rodata))   KEEP(*(.ov2.data)) }
    /* ... one entry per module ... */
  } >OVL AT>FLASH

  /* Load-image symbols consumed by the application's scl_overlay_module table:
   * flash load address (LMA) and byte size of each module, plus the shared
   * window VMA. (GNU ld also auto-defines __load_start_ovN/__load_stop_ovN;
   * deriving from LOADADDR/SIZEOF avoids name-mangling ambiguity.) */
  _ov1_lma = LOADADDR(.ov1);  _ov1_size = SIZEOF(.ov1);
  _ov2_lma = LOADADDR(.ov2);  _ov2_size = SIZEOF(.ov2);
  _ovl_vma = ADDR(.ov1);      /* shared run address == ORIGIN(OVL) */

/* The application then builds its table:
 *
 *   extern uint8_t _ovl_vma[], _ov1_lma[], _ov1_size[], _ov2_lma[], _ov2_size[];
 *   static const scl_overlay_module mods[] = {
 *       { _ovl_vma, _ov1_lma, (uint32_t)(uintptr_t)_ov1_size, "ov1" },
 *       { _ovl_vma, _ov2_lma, (uint32_t)(uintptr_t)_ov2_size, "ov2" },
 *   };
 *   scl_overlay_region ovl;
 *   scl_overlay_init(&ovl, mods, 2);
 *   scl_overlay_load(&ovl, 0);
 */
