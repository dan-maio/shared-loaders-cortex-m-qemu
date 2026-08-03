/**
 * @file elf-image.hpp
 * @brief Parses and provides typed access to an ARM ELF32-LE object.
 */
#pragma once
#include "loader/byte-source.hpp"
#include "loader/elf-types.hpp"
#include <string_view>
#if defined(SCL_HOSTED)
#include <vector>
#endif

namespace loader {

/// @brief Validated view over an ARM ELF32 shared object read via a byte_source.
/// @note The byte_source passed to parse() is referenced by pointer; the caller
///       must ensure it outlives the elf_image.
class elf_image {
public:
    /**
     * @brief Validate magic/class/data/machine and read the ELF header.
     * @param src Byte source positioned at the start of the object.
     * @return The parsed image, or an Error.
     * @retval error::NOT_ELF          Bad magic.
     * @retval error::NOT_ARM32        Not little-endian 32-bit ARM.
     */
    static result<elf_image> parse(byte_source& src) noexcept;

    /// @brief The validated ELF file header.
    [[nodiscard]] const elf::Ehdr& header() const noexcept { return ehdr_; }

    /// @brief Read program header @p index into @p out.
    [[nodiscard]] result<void> program_header(elf::half index, elf::Phdr& out) const noexcept;

    /// @brief Read section header @p index into @p out.
    [[nodiscard]] result<void> section_header(elf::half index, elf::Shdr& out) const noexcept;

    /// @brief Find a section header by name; returns error::BAD_SECTION if absent.
    [[nodiscard]] result<elf::Shdr> find_section(std::string_view name) const noexcept;

    /// @brief Total in-memory size of all PT_LOAD segments (file + bss), 32-bit safe.
    [[nodiscard]] result<std::uint32_t> image_size() const noexcept;

    /// @brief Image base address (lowest PT_LOAD p_vaddr).
    [[nodiscard]] result<std::uint32_t> image_base() const noexcept;

#if defined(SCL_HOSTED)
    /// @brief Concatenate the contents of every allocated SHT_REL section
    ///        (.rel.dyn, .rel.plt, ...) into one buffer, in section order.
    ///        Hosted-only (std::vector): host tests and the static loader;
    ///        freestanding firmware uses reloc_applier over raw spans instead.
    [[nodiscard]] result<std::vector<std::byte>> collect_relocations() const noexcept;
#endif

    /// @brief Access the underlying byte source (for reading section contents).
    [[nodiscard]] byte_source& source() const noexcept { return *src_; }

private:
    elf_image(byte_source& src, const elf::Ehdr& h) noexcept : src_(&src), ehdr_(h) {}
    byte_source* src_;
    elf::Ehdr   ehdr_;
};

} // namespace loader
