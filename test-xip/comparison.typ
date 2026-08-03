#set page(margin: 2.5cm)
#set text(font: "DejaVu Sans Mono", size: 8pt)
#set table(stroke: 0.5pt)

#align(center, text(size: 14pt, weight: "bold", "XIP vs RAM Execution — Library Comparison"))

== Traditional nolib loader (all-RAM baseline)
Every `.so` loaded #text(fill: blue, "entirely in RAM"): code, rodata, PLT, GOT, data, bss — all copied from flash to RAM.

== XIP adapter (final — all 10 libraries XIP)
All libraries rebuilt with `xip-split.ld` (ALIGN(16)), `-msingle-pic-base -mpic-register=r10`, `-Bsymbolic`. Every library has two PT_LOAD segments with VA==file_offset → `split==true`.

#table(
  columns: (auto, auto, auto),
  inset: 4pt,
  [*Region*], [*Traditional nolib*], [*This test*],
  [`.text` code], [RAM], [#text(fill: red, "flash (0x0804xxxx)")],
  [`.rodata` strings], [RAM], [#text(fill: red, "flash")],
  [`.plt`], [RAM], [#text(fill: red, "flash")],
  [`.got`], [RAM], [RAM (0x2000xxxx)],
  [`.data`], [RAM], [RAM],
  [`.bss`], [RAM], [RAM],
  [`.ARM.exidx`], [RAM], [#text(fill: red, "flash")],
)

== All 10 libraries — XIP
#table(
  columns: (auto, auto, auto, auto),
  inset: 4pt,
  [*Library*], [*Text in flash*], [*Data in RAM*], [*RAM saved*],
  [mylib1.so], [3248 B], [304 B], [3248 B],
  [mylib2.so], [1096 B], [256 B], [1096 B],
  [c1.so], [1332 B], [272 B], [1332 B],
  [c2.so], [1708 B], [276 B], [1708 B],
  [cpp3.so], [2784 B], [317 B], [2784 B],
  [libprov.so], [86 B], [108 B], [86 B],
  [libcons.so], [164 B], [144 B], [164 B],
  [libcppeh.so], [1964 B], [284 B], [1964 B],
  [libcppsh.so], [2196 B], [269 B], [2196 B],
  [ehx_catch.so], [780 B], [192 B], [780 B],
  [*Total*], [*15 358 B*], [*2 422 B*], [*~15 KB*],
)

== Known limitation
C++ EH (`eh_selftest`, `ehx_catch_cross`) skipped: `xip_cross_call`'s `.ARM.exidx` has compact model 0 (CANTUNWIND) — the unwinder cannot traverse the trampoline frame.  Same limitation as `dl_cross_call` in the nolib loader.
