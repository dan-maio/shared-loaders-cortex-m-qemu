# test-static — Static GOT-Patcher Tests

This directory contains tests for the runtime ELF32 static patcher (`scl::static`).

## Context
Unlike dynamic loading which resolves symbols and offsets on-demand at runtime, the static patcher resolves and patches relocations and GOT/PLT offsets of pre-linked position-independent libraries at program boot time.

## What (Specific Tests)
* **f411ce-sh**: Compiles two sample libraries (`libsh1.so` and `libsh2.so`) and a main firmware that invokes the static patcher at boot to link, relocate, and execute functions from these libraries.

## How (Execution)
Compile and run the static patcher tests via:
```bash
./test/build_and_run.sh static
```
