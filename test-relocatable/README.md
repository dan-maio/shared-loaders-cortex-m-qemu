# test-relocatable — Relocatable Loader Test Tree

Test firmware applications that exercise the relocatable ELF object (`scl::reloc`) loader.

## Context
This test tree validates the loading of relocatable objects (`.rel.o` / `ET_REL`) rather than shared libraries (`.so` / `ET_DYN`). Relocatable loading bypasses the need for Global Offset Tables (GOT) and Procedure Linkage Tables (PLT) by placing individual ELF sections (`.text`, `.data`, `.bss`) into a RAM arena and applying standard absolute/relative relocations (such as `R_ARM_ABS32`, `R_ARM_TARGET1`, `R_ARM_THM_CALL`) directly.

## Test Targets

| Target | App | Description |
|--------|-----|-------------|
| `reloc` | `scl-fw-reloc` | 9-check battery: relprov+relmod, cyclic c1+c2/cpp3 via `load_set` |
| `classic` | `scl-fw-classic-reloc` | Classic relocatable demo with flatfs blob embedding |
| `classic-spi` | `scl-fw-classic-spi-reloc` | Relocatable loading from SPI flash storage |
| `interrupt` | `scl-fw-reloc-interrupt` | ET_REL modules with interrupt handler registration |

## How to run
All tests under this directory can be compiled and executed using the main orchestrator script:
```bash
./test-relocatable/build-and-run.sh reloc          # 9-check battery
./test-relocatable/build-and-run.sh classic         # flatfs demo
./test-relocatable/build-and-run.sh classic-spi     # SPI flash demo
./test-relocatable/build-and-run.sh interrupt       # interrupt handler test
```

For a list of valid options, run the script without any arguments:
```bash
./test-relocatable/build-and-run.sh
```
