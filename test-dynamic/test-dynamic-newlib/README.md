# test-dynamic-newlib — Hosted Dynamic Loader Tests

This directory contains dynamic loader tests executed in a hosted environment (linking against Newlib standard library, providing heap allocations and standard I/O) using the `scl::dynamic-newlib` loader.

## Context
Hosted loading enables standard C/C++ libraries (like `std::vector`, heap storage, exceptions, and `printf` support) inside the loader itself and loaded modules. This suite tests stability and error handling when loading complex C++ objects.

## What (Specific Tests)
* **test1 (`scl-fw-dynamic-newlib1`)**: Validates the SCL dynamic linker core, including journal rollback (on load failure), memory allocation boundary checks, and error logging.
* **test2 (`scl-fw-dynamic-newlib2`)**: Runs the full cross-library calling test battery (equivalent to freestanding `test2`/`test3` but using the hosted loader).

## How (Execution)
Run these tests using either the soft-float (`sf`) or hard-float (`hf`) wrapper variants:
```bash
./test/build_and_run.sh [newlib1-sf|newlib2-sf|newlib1-hf|newlib2-hf]
```
