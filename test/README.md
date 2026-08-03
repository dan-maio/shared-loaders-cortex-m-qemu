# test — Dynamic Loader Test Tree

Test firmware applications that exercise the standard dynamic shared-library (`scl::dynamic-nolib`, `scl::dynamic-newlib`, and `scl::overlay`) loaders.

## Context
This test tree validates runtime ELF32 loading, dynamic linking, relocation resolution, and constructor execution on Cortex-M4 hardware (specifically STM32F411RE). It includes tests for both freestanding environments (no standard library) and hosted environments (linking against Newlib, with heap and C++ exception support).

## What it does
* **Freestanding (Nolib) Tests**: Evaluates basic `dlopen`, `dlsym`, and `dlclose` functionality, lazy resolution, and overlay comparisons.
* **Hosted (Newlib) Tests**: Exercises full C/C++ cross-library calling, global object constructor/destructor execution, and dynamic exceptions/RTTI.
* **Static Patcher**: Exercises the standalone static GOT-patcher (`scl::static`) that patches relocations at program boot.

## How to run
The firmware apps live in the per-technique trees (`test-dynamic/`,
`test-overlay/`, `test-static/`, `test-relocatable/`, `test-xip/`). Use the
repo-root orchestrator to build, flash and run one app on either board:

```bash
./build-and-run.sh board=<nucleo|weact> test=<name>
```

Run it without arguments for the full test-name list. `board=` selects the
target board (NUCLEO-F411RE = ST-LINK, WEACT-F411CE = CMSIS-DAP); only board
boot/init differs between them. Each per-tree `build-and-run.sh` also accepts
the `SCL_BOARD` environment variable for the same selection.
