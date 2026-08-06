# test-dynamic-nolib — Freestanding Dynamic Loader Tests

This directory contains dynamic loader tests executed in a freestanding bare-metal environment (no C standard library, no heap allocations) using the `scl::dynamic-nolib` loader.

## Context
These tests validate runtime loading and execution of position-independent shared libraries (`.so`) without standard library support. Relocations and symbol resolutions are handled on-chip using pure bare-metal hooks.

## What (Specific Tests)
* **test1 (`scl-fw-dyn1`)**: Basic `dlopen`, `dlsym`, and `dlclose` routines targeting two dynamic libraries (`mylib1.so`, `mylib2.so`), C++ exception handling (`libcppeh.so`), and C++ dynamic class instances (`libcppsh.so`).
* **test2 (`scl-fw-dyn2`)**: A 7-check cross-module calling battery linking three dynamic libraries (`c1.so`, `c2.so`, `cpp3.so`). Validates lazy binding, inter-library references, and prints GOT layouts.
* **test3 (`scl-fw-flatfs`)**: Reruns the `test2` battery but loads the dynamic libraries from an on-chip Flash-based archive (flat filesystem).
* **test4 (`scl-fw-gotplt-demo`)**: A didactic walkthrough showing how the dynamic loader intercepts PLT calls and populates the GOT step-by-step.

## How (Execution)
Run any of the tests using the primary workspace script:
```bash
./test/build-and-run.sh [dyn1|dyn2|flatfs|gotplt-demo]
```
