/**
 * @file multi_c.cpp
 * @brief Leaf file of the 3-file -r bundle. Defines the shared global and the
 *        innermost helper. `multi_shared` is exported (extern "C", unmangled);
 *        `c_add` is an internal C++ symbol resolved *inside* the bundle by ld -r.
 */
extern "C" { int multi_shared = 100; }        // exported data symbol

int c_add(int x) { return x + multi_shared; } // internal; called by multi_b
