/**
 * @file battery.hpp
 * @brief Shared entry points so the flat_fs driver (main-flatfs.cpp) can reuse
 *        the exact 15-test battery defined in main-tests.cpp.
 *
 * The battery body is identical in both firmwares; only the SOURCE of the
 * library bytes differs. main-tests.cpp opens libraries through open_sel(),
 * which consults g_use_flatfs:
 *   - false (default, stock battery): fw::dlopen(path)        -- host semihosting
 *   - true  (flat_fs build sets it):  fw::dlopen_fs(basename) -- on-chip flash FS
 */
#ifndef BATTERY_HPP_
#define BATTERY_HPP_

/// @brief When true, the battery loads libraries from the on-chip flat_fs
///        (dlopen_fs by basename) instead of host semihosting paths.
extern bool g_use_flatfs;

/// @brief Run the 15-test cross-module battery. Defined in main-tests.cpp.
int battery_run();

#endif /* BATTERY_HPP_ */
