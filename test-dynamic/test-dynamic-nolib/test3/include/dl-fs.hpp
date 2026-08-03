/**
 * @file dl-fs.hpp
 * @brief flat_fs app front-end: load a library by name from the on-chip flat
 *        archive (Flashbyte_source over the blob -> the SCL's dlopen_source).
 *        Implemented in dl-fs.cpp; this is application code, not SCL code.
 */
#pragma once
#include "dl.hpp"

namespace fw {
/** @brief Load @p name from the on-chip flat archive (FileNotFound if absent). */
[[nodiscard]] loader::result<handle*> dlopen_fs(const char* name) noexcept;
} // namespace fw
