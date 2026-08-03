/**
 * @file dl-load.hpp
 * @brief Application-side load front-end: slurp a .so over semihosting (ONE
 *        read) and feed it to the SCL dynamic_linker's load_from_source().
 *        The SCL never assumes a storage backend; this is this app's backend.
 */
#pragma once
#include "dynamic-linker.hpp"

namespace firmware {
/** @brief Open @p path over semihosting and load it via @p linker. */
[[nodiscard]] loader::result<library*>
load_semihost(dynamic_linker& linker, const char* path) noexcept;
} // namespace firmware
