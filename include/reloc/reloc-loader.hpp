/**
 * @file reloc-loader.hpp
 * @brief scl::reloc — loader for ELF32 **relocatable** (ET_REL) modules.
 *
 * A companion to the ET_DYN loaders (scl::dynamic-nolib / dynamic-newlib /
 * static). Where those consume `-fPIC -shared` objects with a GOT/PLT and
 * dynamic relocations, this consumes `-mlong-calls -fno-pic -r` objects — plain
 * relocatable `.o`s (like a Linux kernel module):
 *
 *   - No GOT, no PLT, no `_DYNAMIC`. Section headers + `.symtab`/`.strtab`.
 *   - `-fno-pic` => the module runs at fixed absolute addresses; every external
 *     reference is an **R_ARM_ABS32** literal (`ldr rN,=target ; blx rN`), so no
 *     `r10`/trampoline and no >BL-range problem (that is what `-mlong-calls`
 *     buys). A handful of same-module calls are **R_ARM_THM_CALL**.
 *
 * The loader: places every SHF_ALLOC section in a caller-supplied RAM arena,
 * resolves `.symtab` (defined => section_base + st_value; UND => the resolver),
 * applies R_ARM_ABS32 / R_ARM_TARGET1 / R_ARM_REL32 / R_ARM_THM_CALL, then runs
 * `.init_array`. Exported symbols are then callable as plain function pointers.
 */
#ifndef RELOC_RELOC_LOADER_HPP
#define RELOC_RELOC_LOADER_HPP

#include "loader/error.hpp"
#include "loader/reloc-applier.hpp"   // loader::symbol_source
#include <cstdint>
#include <span>
#include <string_view>

namespace reloc {

/// Fixed cap on section-header count (freestanding: no heap for the base table).
inline constexpr int MAX_SECTIONS = 48;
/// Fixed cap on modules linked together by load_set().
inline constexpr int MAX_SET = 4;

/**
 * @brief A loaded ET_REL module. Its SHF_ALLOC sections live in @ref arena;
 *        `sec_base[i]` is the runtime address of section @c i (0 if unallocated).
 *        Keeps the original @ref image so exported symbols can be looked up
 *        (`.symtab`/`.strtab` are read from it on demand).
 */
struct module {
    std::span<std::byte>       arena{};       ///< RAM the sections were placed in.
    std::span<const std::byte> image{};       ///< Original ET_REL file bytes.
    std::uint32_t sec_base[MAX_SECTIONS]{};   ///< Runtime base per section index.
    std::uint32_t used{};                     ///< Bytes of @ref arena consumed.
    std::uint16_t shnum{};                    ///< Section-header count.
    std::uint32_t shoff{};                    ///< Section-header table offset.
    std::uint16_t shentsize{};                ///< Section-header entry size.
    std::uint16_t symtab_idx{};               ///< Section index of `.symtab`.
    bool valid{false};

    explicit operator bool() const noexcept { return valid; }
};

/**
 * @brief Load an ET_REL @p image into @p arena, resolving undefined symbols via
 *        @p resolve. Applies relocations and runs `.init_array`.
 * @return The loaded @ref module, or an error (BAD_SECTION / UNSUPPORTED_RELOC /
 *         OUT_OF_MEMORY / NOT_ELF / NOT_ARM32).
 */
loader::result<module> load(std::span<const std::byte> image,
                            std::span<std::byte>       arena,
                            loader::symbol_source      resolve) noexcept;

/// @brief Runtime address of an exported (defined, non-UND) symbol, or 0.
std::uint32_t sym(const module& m, std::string_view name) noexcept;

/**
 * @brief A group of ET_REL modules linked together (@ref load_set). Needed when
 *        the modules have mutual/cyclic imports: single-module @ref load cannot
 *        resolve a reference to a symbol in a not-yet-loaded module.
 */
struct module_set {
    module mods[MAX_SET]{};
    int    n{0};
    explicit operator bool() const noexcept { return n > 0; }
};

/**
 * @brief Load a SET of ET_REL modules together, resolving cross-module symbols.
 *
 * Two phases, so mutual/cyclic dependencies work (unlike single @ref load):
 *   1. **place** every module's SHF_ALLOC sections in @p arena (contiguous) —
 *      now every symbol in every module has an address;
 *   2. **resolve** each module's relocations against the whole set first, then
 *      @p resolve (main) for anything no module defines; then run all ctors.
 * @return The @ref module_set, or an error.
 */
loader::result<module_set> load_set(std::span<const std::span<const std::byte>> images,
                                    std::span<std::byte>                        arena,
                                    loader::symbol_source                       resolve) noexcept;

/// @brief Runtime address of an exported symbol searched across all modules, or 0.
std::uint32_t sym(const module_set& s, std::string_view name) noexcept;

}  // namespace reloc

#endif  // RELOC_RELOC_LOADER_HPP
