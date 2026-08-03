/**
 * @file reloc-applier.hpp
 * @brief Streams an ARM .rel table and applies it into a loaded, writable image.
 *
 * Unlike relocator (which builds a std::vector relocation_plan, host-only), this
 * applier is allocation-free and STL-light so it compiles for the freestanding
 * Cortex-M target: it reads each elf::Rel directly and patches the image. The
 * external-symbol resolver is a plain function pointer + context (no std::function).
 */
#pragma once
#include "loader/elf-types.hpp"
#include "loader/error.hpp"
#include <cstdint>
#include <span>
#include <string_view>

namespace loader {

/** @brief Resolve an external symbol name to an absolute address (e.g. in main). */
struct symbol_source {
    result<std::uint32_t> (*fn)(void* ctx, std::string_view name);
    void* ctx;
    result<std::uint32_t> operator()(std::string_view n) const { return fn(ctx, n); }
};

class reloc_applier {
public:
    /**
     * @brief Apply a raw .rel(.dyn/.plt) table into @p image.
     * @param image  Writable bytes of the loaded image (vaddr 0 at image[0]).
     * @param base   Runtime address of image[0] (== load bias).
     * @param reldyn Bytes of the .rel table (array of elf::Rel).
     * @param dynsym Bytes of .dynsym (array of elf::Sym).
     * @param dynstr Bytes of .dynstr (NUL-separated names).
     * @param resolve Resolver for external symbols (st_value == 0).
     * @note Defined symbols (st_value != 0) bind to base+st_value. Unresolved
     *       externals are written as 0 (matching --unresolved-symbols=ignore).
     * @retval error::UNSUPPORTED_RELOC A relocation type is not handled.
     * @retval error::BAD_SECTION      A target offset is outside the image.
     */
    static result<void> apply_rel(std::span<std::byte> image, std::uint32_t base,
                                  std::span<const std::byte> reldyn,
                                  std::span<const std::byte> dynsym,
                                  std::span<const std::byte> dynstr,
                                  symbol_source resolve) noexcept;
};

} // namespace loader
