/**
 * @file cross-error.hpp
 * @brief A library-defined exception type, compiled into BOTH the loadable
 *        library and main so a throw in the lib can be caught by type in main.
 *
 * The literal `throw std::runtime_error(...)` form does NOT link under -nostdlib:
 * the catch type table emits an R_ARM_REL32 against the *undefined-external*
 * `_ZTISt13runtime_error`, which `ld` rejects ("dangerous relocation"). A type
 * defined here has *local* typeinfo in each module; cross-module catch matching
 * then succeeds by typeinfo NAME comparison (the bare-metal GNU default), which
 * is exactly the cross-module RTTI unification Plan 5d demonstrates.
 */
#ifndef CROSS_ERROR_HPP_
#define CROSS_ERROR_HPP_

#include <cstdint>

/// @brief Trivial carrier exception with an integer code (no heap, no std deps).
class cross_error {
public:
    explicit cross_error(std::uint32_t code) noexcept : code_(code) {}
    std::uint32_t code() const noexcept { return code_; }
private:
    std::uint32_t code_;
};

#endif /* CROSS_ERROR_HPP_ */
