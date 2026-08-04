/**
 * @file dl-semihost.hpp
 * @brief Application-side dlopen front-end: open @p path over semihosting and
 *        feed it to the SCL's dlopen_source(). The SCL itself never assumes a
 *        storage backend; this wrapper is the test apps' backend.
 */
#pragma once
#include "dl.hpp"

namespace fw {
/** @brief Load @p path over semihosting: copy, relocate, resolve, run ctors. */
[[nodiscard]] loader::result<handle*> dlopen(const char* path) noexcept;
} // namespace fw
