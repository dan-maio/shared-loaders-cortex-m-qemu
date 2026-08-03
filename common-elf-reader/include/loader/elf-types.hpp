/**
 * @file elf-types.hpp
 * @brief ARM ELF32 little-endian on-disk structures, constants, and decoders.
 *
 * The core is deliberately hard-wired to ARM ELF32-LE as a *data format*. These
 * structs mirror the ELF ABI layout exactly so they can be memcpy'd out of a
 * byte buffer on any little-endian host or on the Cortex-M target.
 */
#pragma once
#include <bit>
#include <cstdint>

namespace loader::elf {

// The on-disk ELF32 structs are memcpy'd directly out of byte buffers, which is
// only correct when the host byte order matches the little-endian file format.
static_assert(std::endian::native == std::endian::little,
              "loader core requires a little-endian host/target (ARM Cortex-M, x86)");

using addr = std::uint32_t;  ///< Unsigned program address.
using off  = std::uint32_t;  ///< File offset.
using word = std::uint32_t;  ///< Unsigned 32-bit word.
using half = std::uint16_t;  ///< Unsigned 16-bit half-word.

/// @brief ELF file header (52 bytes).
struct Ehdr {
    unsigned char e_ident[16]; ///< Magic + class/data/version identification.
    half e_type;       ///< Object file type (ET_DYN expected).
    half e_machine;    ///< Architecture (EM_ARM expected).
    word e_version;    ///< Object file version.
    addr e_entry;      ///< Entry point (0 for these libraries).
    off  e_phoff;      ///< Program header table file offset.
    off  e_shoff;      ///< Section header table file offset.
    word e_flags;      ///< Processor-specific flags.
    half e_ehsize;     ///< ELF header size in bytes.
    half e_phentsize;  ///< Program header entry size.
    half e_phnum;      ///< Program header entry count.
    half e_shentsize;  ///< Section header entry size.
    half e_shnum;      ///< Section header entry count.
    half e_shstrndx;   ///< Section header string table index.
};

/// @brief Program header (32 bytes).
struct Phdr {
    word p_type;    ///< Segment type (PT_LOAD relevant here).
    off  p_offset;  ///< Segment file offset.
    addr p_vaddr;   ///< Segment virtual address.
    addr p_paddr;   ///< Segment physical address.
    word p_filesz;  ///< Bytes of the segment in the file.
    word p_memsz;   ///< Bytes of the segment in memory (>= filesz; bss zero-fill).
    word p_flags;   ///< Segment flags.
    word p_align;   ///< Segment alignment.
};

/// @brief Section header (40 bytes).
struct Shdr {
    word sh_name;       ///< Section name (index into section string table).
    word sh_type;       ///< Section type.
    word sh_flags;      ///< Section attributes.
    addr sh_addr;       ///< Virtual address in memory.
    off  sh_offset;     ///< Offset in file.
    word sh_size;       ///< Size in bytes.
    word sh_link;       ///< Link to another section.
    word sh_info;       ///< Additional section information.
    word sh_addralign;  ///< Address alignment.
    word sh_entsize;    ///< Entry size if the section holds a table.
};

/// @brief Dynamic symbol table entry (16 bytes).
struct Sym {
    word st_name;          ///< Symbol name (index into .dynstr).
    addr st_value;         ///< Symbol value (image-relative address; 0 => external).
    word st_size;          ///< Symbol size in bytes.
    unsigned char st_info; ///< Binding (high nibble) and type (low nibble).
    unsigned char st_other;///< Visibility.
    half st_shndx;         ///< Section index (SHN_UNDEF => external).
};

/// @brief REL relocation entry (8 bytes; ARM uses REL, not RELA).
struct Rel {
    addr r_offset;  ///< Location to apply the relocation.
    word r_info;    ///< Symbol index (high 24 bits) and type (low 8 bits).
};

/// @brief Dynamic-section entry (8 bytes); array terminated by DT_NULL.
struct Dyn {
    std::int32_t d_tag;  ///< Dynamic entry type (DT_*).
    word         d_val;  ///< Value or address (d_val/d_ptr union; same width here).
};

constexpr std::int32_t DT_NULL          = 0;   ///< End of _DYNAMIC array.
constexpr std::int32_t DT_PLTGOT        = 3;   ///< Address of the PLT/GOT.
constexpr std::int32_t DT_HASH          = 4;   ///< Address of the symbol hash table.
constexpr std::int32_t DT_STRTAB        = 5;   ///< Address of the string table.
constexpr std::int32_t DT_SYMTAB        = 6;   ///< Address of the symbol table.
constexpr std::int32_t DT_STRSZ         = 10;  ///< Size in bytes of the string table.
constexpr std::int32_t DT_SYMENT        = 11;  ///< Size of one symbol table entry.
constexpr std::int32_t DT_INIT_ARRAY    = 25;  ///< Address of the init function array.
constexpr std::int32_t DT_FINI_ARRAY    = 26;  ///< Address of the fini function array.
constexpr std::int32_t DT_INIT_ARRAYSZ  = 27;  ///< Size in bytes of the init array.
constexpr std::int32_t DT_FINI_ARRAYSZ  = 28;  ///< Size in bytes of the fini array.

// --- Identification constants ---
constexpr unsigned char ELFMAG0 = 0x7f; ///< e_ident[0].
constexpr unsigned char ELFMAG1 = 'E';  ///< e_ident[1].
constexpr unsigned char ELFMAG2 = 'L';  ///< e_ident[2].
constexpr unsigned char ELFMAG3 = 'F';  ///< e_ident[3].
constexpr unsigned char ELFCLASS32  = 1; ///< e_ident[EI_CLASS] for 32-bit.
constexpr unsigned char ELFDATA2LSB = 1; ///< e_ident[EI_DATA] for little-endian.
constexpr int EI_CLASS = 4; ///< Index of the class byte.
constexpr int EI_DATA  = 5; ///< Index of the data-encoding byte.

constexpr half ET_REL = 1;  ///< Relocatable file type.
constexpr half ET_EXEC = 2; ///< Executable file type.
constexpr half ET_DYN = 3;  ///< Shared object file type.
constexpr half EM_ARM = 40; ///< ARM architecture (0x28).

constexpr word PT_LOAD = 1; ///< Loadable program segment.

constexpr word SHT_REL   = 9; ///< Relocation section (Elf32_Rel entries).
constexpr word SHF_ALLOC = 2; ///< Section occupies memory during execution.

/// @brief ARM REL relocation types the loader recognises.
enum class rel_type : std::uint8_t {
    ABS32    = 2,  ///< R_ARM_ABS32 (e.g. constructor pointers).
    REL32    = 3,  ///< R_ARM_REL32 (PC-relative 32-bit data reference).
    COPY     = 20, ///< R_ARM_COPY (extern var defined in main).
    GLOB_DAT = 21, ///< R_ARM_GLOB_DAT (global variable GOT slot).
    JUMP_SLOT = 22, ///< R_ARM_JUMP_SLOT (function GOT/PLT slot).
    RELATIVE = 23, ///< R_ARM_RELATIVE (base-relative local data).
};

/// @brief Symbol binding (high nibble of st_info).
enum class bind : std::uint8_t {
    LOCAL  = 0, ///< STB_LOCAL.
    GLOBAL = 1, ///< STB_GLOBAL.
    WEAK   = 2, ///< STB_WEAK.
};

/// @brief Extract the symbol-table index from a relocation's r_info.
constexpr word        r_sym (word info) noexcept { return info >> 8; }
/// @brief Extract the relocation type from a relocation's r_info.
constexpr std::uint8_t r_type(word info) noexcept { return static_cast<std::uint8_t>(info & 0xffu); }
/// @brief Extract the binding from a symbol's st_info.
constexpr std::uint8_t st_bind(std::uint8_t info) noexcept { return info >> 4; }
/// @brief Extract the symbol type from a symbol's st_info.
constexpr std::uint8_t st_type(std::uint8_t info) noexcept { return info & 0xfu; }

} // namespace loader::elf
