/**
 * @file dl-impl.hpp
 * @brief Target-side implementation of the dl:: facade.
 *
 * Translates core std::expected errors into the POSIX
 * nullptr + dlerror() convention.
 */
#pragma once
#include "dynamic-linker.hpp"
#include "link-context.hpp"

namespace dl {

/** @brief Opaque handle representing a loaded library (pointer to library). */
using handle = firmware::library*;

/** @brief dlopen mode flags (minimal: all behave as RTLD_NOW). */
enum class Mode { Now, Global, Local };

/** @brief Load a shared library: parse, resolve, relocate, run constructors. */
[[nodiscard]] handle      dlopen (const char* path, Mode mode = Mode::Now) noexcept;

/** @brief Look up a symbol by name in a loaded library, searching all loaded libs. */
[[nodiscard]] void*       dlsym  (handle h, const char* name) noexcept;

/** @brief Unload a library: revert relocations, free memory, remove from list. */
int                       dlclose(handle h) noexcept;

/** @brief Return the last error string (nullptr if no error) and clear it. */
[[nodiscard]] const char* dlerror() noexcept;

} // namespace dl
