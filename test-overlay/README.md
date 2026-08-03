# test-overlay — Linker Overlay Tests

This directory contains linker overlay tests using the `scl::overlay` manager.

## Context
Linker overlays allow mapping multiple code sections (e.g. separate modules) to the same physical SRAM address window. Only one overlay segment resides in SRAM at a time, allowing applications to exceed physical RAM limits by swapping code segments in and out of Flash on demand.

## What (Specific Tests)
* **test1 (`scl-fw-ovl1`)**: Compares memory consumption and overhead between a standard linker overlay implementation and the dynamic loader (`scl::dynamic-nolib`).

## How (Execution)
Compile, flash, and run the overlay test via:
```bash
./test/build_and_run.sh ovl1
```
