# test-xip/common — shared XIP-test platform helpers

Files that are **byte-identical** across all three XIP test trees
(`test-dynamic-nolib/test1`, `test-dynamic-newlib/test1`, `test-static`) live
here so there is a single copy. Each test's `CMakeLists.txt` references them via
`${_repo}/test-xip/common/...`.

| file | role |
| --- | --- |
| `src/semihost-file.cpp` | semihosting file reader (provisioning reads each `.so` over semihosting) |
| `include/abort_fix.h` | force-included (`-include`) abort/EH shim, same for every XIP app |

Per-loader files that **differ** (each `main.cpp`, `test-helpers.hpp`,
`test_c1_c2_cpp3.cpp`, `test_ehx_cons_ppeh_ppsh_prov.cpp`, `walkthrough.cpp`,
`sections-flatfs-hosted.ld`, and the static loader) stay in their own tree.
