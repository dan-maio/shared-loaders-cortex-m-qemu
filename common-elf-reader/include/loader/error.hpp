/**
 * @file error.hpp
 * @brief Error codes and the result type used throughout the portable core.
 */
#pragma once
#include <optional>
#include <utility>

namespace loader {

/// @brief Failure modes reported by the portable loader core.
enum class error {
    OK,                ///< No error (not normally stored in an unexpected).
    FILE_NOT_FOUND,    ///< Byte source could not be opened.
    SHORT_READ,        ///< Byte source returned fewer bytes than requested.
    NOT_ELF,           ///< Magic number is not 0x7f 'E' 'L' 'F'.
    NOT_ARM32,         ///< Not a little-endian 32-bit ARM object.
    NOT_SHARED_OBJECT, ///< e_type is not ET_DYN.
    BAD_SECTION,       ///< Required section missing or malformed.
    SYMBOL_NOT_FOUND,  ///< Hash lookup did not find the requested name.
    UNSUPPORTED_RELOC, ///< Relocation type the loader does not implement.
    OUT_OF_MEMORY,     ///< A fixed-capacity container overflowed.
};

/// @brief Wrapper marking an error as the failure alternative of a result.
///        C++20 stand-in for std::unexpected (the SCL does not use C++23).
struct unexpect_t {
    error err;
};

/// @brief Build the failure alternative of a result from an error.
[[nodiscard]] constexpr unexpect_t unexpected(error e) noexcept { return unexpect_t{e}; }

/**
 * @brief A value of type T or an error — minimal C++20 replacement for
 *        std::expected<T, error>, exposing only the operations the SCL uses:
 *        operator bool / has_value, operator* / operator->, err(), value_or().
 * @tparam T The success type.
 */
template <class T>
class result {
public:
    result(T&& v) : val_(std::move(v)) {}
    result(const T& v) : val_(v) {}
    result(unexpect_t u) noexcept : err_(u.err) {}

    [[nodiscard]] bool has_value() const noexcept { return val_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return val_.has_value(); }

    [[nodiscard]] T&       operator*() &  noexcept { return *val_; }
    [[nodiscard]] const T& operator*() const& noexcept { return *val_; }
    [[nodiscard]] T&&      operator*() && noexcept { return *std::move(val_); }
    [[nodiscard]] T*       operator->() noexcept { return &*val_; }
    [[nodiscard]] const T* operator->() const noexcept { return &*val_; }

    /// @brief The failure code; only meaningful when !has_value().
    [[nodiscard]] ::loader::error err() const noexcept { return err_; }

    /// @brief The contained value; must only be called when has_value().
    [[nodiscard]] T&       value() &  noexcept { return *val_; }
    [[nodiscard]] const T& value() const& noexcept { return *val_; }

    template <class U>
    [[nodiscard]] T value_or(U&& fallback) const& {
        return val_.has_value() ? *val_ : static_cast<T>(std::forward<U>(fallback));
    }

private:
    std::optional<T> val_{};
    ::loader::error  err_ = error::OK;
};

/// @brief result<void>: success carries no value.
template <>
class result<void> {
public:
    result() noexcept = default;
    result(unexpect_t u) noexcept : err_(u.err), ok_(false) {}

    [[nodiscard]] bool has_value() const noexcept { return ok_; }
    [[nodiscard]] explicit operator bool() const noexcept { return ok_; }
    [[nodiscard]] ::loader::error err() const noexcept { return err_; }

private:
    ::loader::error err_ = error::OK;
    bool  ok_  = true;
};

} // namespace loader
