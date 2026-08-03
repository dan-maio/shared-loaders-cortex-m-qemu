# XIP vs RAM Execution — Library Comparison

## Traditional nolib loader (all-RAM baseline)

Every `.so` loaded **entirely in RAM**: code, rodata, PLT, GOT, data, bss — all copied from flash to RAM at `dlopen` time. Both RE and RW segments live at `0x2000xxxx` (RAM).

## XIP adapter (final — all 10 libraries XIP)

All libraries rebuilt with `xip-split.ld` (ALIGN(16) between segments), `-msingle-pic-base -mpic-register=r10`, `-Bsymbolic`. Every library now has two separate PT_LOAD segments with **VA==file_offset** for both, so `split==true` for all.

| Region | Traditional nolib | This test |
|--------|-------------------|-----------|
| `.text` code | RAM | **flash** (`0x0804xxxx`) |
| `.rodata` strings/consts | RAM | **flash** |
| `.plt` | RAM | **flash** |
| `.got` | RAM | RAM (`0x2000xxxx`) |
| `.data` | RAM | RAM |
| `.bss` | RAM | RAM |
| `.ARM.exidx` (EH unwind) | RAM | **flash** |
| Constructors | run from RAM | run from RAM (via `pic_call_void` to fn in flash) |
| External calls | PLT → RAM GOT → resolved | PLT in flash → **flash GOT** (pre-patched to `xip_cross_call`) → trampoline |

---

## Each library

### 1. mylib1.so — XIP (split, VA==off)
Built by `build-fixtures.sh` with `xip-split.ld`.  Segment sizes: text 3248 B, data 304 B.

### 2. mylib2.so — XIP (split, VA==off)
Built by `build-fixtures.sh` with `xip-split.ld`.  Segment sizes: text 1096 B, data 256 B.

### 3. c1.so — XIP (split, VA==off)
Rebuilt by `build-xip-libs.sh`.  Text 1332 B, data 272 B in flash.

### 4. c2.so — XIP (split, VA==off)
Rebuilt by `build-xip-libs.sh`.  Text 1708 B, data 276 B in flash.

### 5. cpp3.so — XIP (split, VA==off)
Rebuilt by `build-xip-libs.sh`.  Text 2784 B, data 317 B in flash.

### 6. libprov.so — XIP (split, VA==off)
Rebuilt by `build-xip-libs.sh`.  Text 86 B, data 108 B in flash.

### 7. libcons.so — XIP (split, VA==off)
Rebuilt by `build-xip-libs.sh`.  Text 164 B, data 144 B in flash.

### 8. libcppeh.so — XIP (split, VA==off)
Rebuilt by `build-xip-libs.sh`.  Text 1964 B, data 284 B in flash.

### 9. libcppsh.so — XIP (split, VA==off)
Rebuilt by `build-xip-libs.sh`.  Text 2196 B, data 269 B in flash.

### 10. ehx_catch.so — XIP (split, VA==off)
Rebuilt by `build-xip-libs.sh`.  Text 780 B, data 192 B in flash.

---

## RAM savings

| Library | Code/rodata in flash | Data/bss in RAM | RAM saved vs all-RAM |
|---------|----------------------|-----------------|----------------------|
| mylib1.so | 3248 B | 304 B | 3248 B |
| mylib2.so | 1096 B | 256 B | 1096 B |
| c1.so | 1332 B | 272 B | 1332 B |
| c2.so | 1708 B | 276 B | 1708 B |
| cpp3.so | 2784 B | 317 B | 2784 B |
| libprov.so | 86 B | 108 B | 86 B |
| libcons.so | 164 B | 144 B | 164 B |
| libcppeh.so | 1964 B | 284 B | 1964 B |
| libcppsh.so | 2196 B | 269 B | 2196 B |
| ehx_catch.so | 780 B | 192 B | 780 B |
| **Total** | **15 358 B** | **2 422 B** | **~15 KB** |

## Known limitation

C++ exception handling (`eh_selftest`, `ehx_catch_cross`) is skipped because `xip_cross_call`'s `.ARM.exidx` entry has compact model 0 (CANTUNWIND).  The `.fnstart/.fnend` directives generate the entry but without a `.personality` reference the unwinder cannot traverse the trampoline frame.  The same limitation affects the nolib's `dl_cross_call` — that test works only because the catch block lives in main (not in a loaded library) and the unwinder stops at the trampoline frame before falling back to main's personality.
