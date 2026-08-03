/**
 * @file dl-impl.cpp
 * @brief dl:: facade implementation.
 *
 * Symbols for automatic resolution come from a table rendered into flash at
 * link time (gen-symtab.py); if that table is absent the firmware falls back to
 * reading its own .symtab from the ELF over semihosting. Either way no symbol
 * is hardcoded.
 */
#include "firmware/dl-impl.hpp"
#include "firmware/dl-load.hpp"
#include "firmware/semihost-file.hpp"
#include "loader/error.hpp"
#include "loader/elf-types.hpp"
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include "diag/trace.h"

/** @brief Path to the firmware's own ELF file on the host.
 *  The .elf is placed next to the .hex by the build system. */
#ifndef FIRMWARE_ELF_PATH
#define FIRMWARE_ELF_PATH "firmware.elf"
#endif

/* Defined by symtab-blob.c; must have external linkage, so it sits outside the
   anonymous namespace below. */
extern "C" const std::uint8_t scl_symtab_blob[];

namespace {

/** @brief Read .symtab from the firmware ELF and intern names into a static buffer. */
static constexpr std::size_t MAX_SYMS = 2048;
static firmware::sym_entry g_symtab[MAX_SYMS];

/* Link-time symbol table in flash (see symtab-blob.c / gen-symtab.py). The
   entry array is already laid out as firmware::sym_entry with absolute name
   pointers, so it can be handed to set_symtab() directly -- no RAM, no heap,
   and no semihosting read at boot. magic == 0 means the post-link rewrite did
   not run, in which case we fall back to reading the ELF. */
static constexpr std::uint32_t kSymtabMagic = 0x534C4353;  /* 'SCLS' */

static const firmware::sym_entry* flash_symtab(std::size_t& count) noexcept {
    std::uint32_t magic, n;
    std::memcpy(&magic, scl_symtab_blob, sizeof(magic));
    std::memcpy(&n, scl_symtab_blob + 4, sizeof(n));
    if (magic != kSymtabMagic || n == 0) return nullptr;
    count = n;
    return reinterpret_cast<const firmware::sym_entry*>(scl_symtab_blob + 8);
}

static std::size_t load_symtab() noexcept {
    trace_printf("[symtab] opening %s\n", FIRMWARE_ELF_PATH);
    firmware::semihost_file elf(FIRMWARE_ELF_PATH);
    if (!elf.is_open()) {
        trace_puts("[symtab] open FAILED\n");
        return 0;
    }
    trace_puts("[symtab] open OK, reading Ehdr\n");

    loader::elf::Ehdr ehdr{};
    if (!elf.read_at(0, {reinterpret_cast<std::byte*>(&ehdr), sizeof(ehdr)}))
        return 0;

    auto shoff = ehdr.e_shoff;
    auto shdr_sz = ehdr.e_shentsize;
    loader::elf::Shdr shstrtab_sh{};
    if (!elf.read_at(shoff + ehdr.e_shstrndx * shdr_sz,
            {reinterpret_cast<std::byte*>(&shstrtab_sh), shdr_sz}))
        return 0;

    loader::elf::Shdr symtab_sh{}, strtab_sh{};
    bool have_sym = false, have_str = false;
    for (unsigned i = 0; i < ehdr.e_shnum; ++i) {
        loader::elf::Shdr sh{};
        if (!elf.read_at(shoff + i * shdr_sz,
                {reinterpret_cast<std::byte*>(&sh), shdr_sz}))
            continue;
        char nm[16] = {};
        if (!elf.read_at(shstrtab_sh.sh_offset + sh.sh_name,
                {reinterpret_cast<std::byte*>(nm), sizeof(nm) - 1}))
            continue;
        if (std::strcmp(nm, ".symtab") == 0) { symtab_sh = sh; have_sym = true; }
        if (std::strcmp(nm, ".strtab") == 0) { strtab_sh = sh; have_str = true; }
    }
    if (!have_sym || !have_str) {
        trace_printf("[symtab] missing: sym=%d str=%d\n", have_sym, have_str);
        return 0;
    }

    // Allocate temporary heap buffer for the entire string table
    char* strtab_buf = reinterpret_cast<char*>(std::malloc(strtab_sh.sh_size));
    if (!strtab_buf) {
        trace_puts("[symtab] out of memory for strtab\n");
        return 0;
    }

    // Read the entire strtab in one semihosting read call
    if (!elf.read_at(strtab_sh.sh_offset, {reinterpret_cast<std::byte*>(strtab_buf), strtab_sh.sh_size})) {
        trace_puts("[symtab] read strtab FAILED\n");
        std::free(strtab_buf);
        return 0;
    }

    auto count = symtab_sh.sh_size / sizeof(loader::elf::Sym);
    trace_printf("[symtab] loading %u symbols (skipping first %u local)\n",
                 (unsigned)count, (unsigned)symtab_sh.sh_info);
    std::size_t n = 0;

    static constexpr std::size_t BLOCK_SIZE = 64;
    static loader::elf::Sym sym_block[BLOCK_SIZE];

    std::size_t start_i = symtab_sh.sh_info;
    if (start_i >= count) start_i = 0;

    for (std::size_t i = start_i; i < count && n < MAX_SYMS; ) {
        std::size_t chunk = count - i;
        if (chunk > BLOCK_SIZE) chunk = BLOCK_SIZE;

        if (!elf.read_at(symtab_sh.sh_offset + i * sizeof(loader::elf::Sym),
                {reinterpret_cast<std::byte*>(sym_block), chunk * sizeof(loader::elf::Sym)})) {
            break;
        }

        for (std::size_t k = 0; k < chunk && n < MAX_SYMS; ++k) {
            const auto& sym = sym_block[k];
            if (sym.st_value == 0) continue;
            if (sym.st_shndx == 0 || sym.st_shndx >= 0xFFF1) continue;
            auto bind = sym.st_info >> 4;  /* ELF64_ST_BIND equivalent */
            if (bind != 1 && bind != 2) continue;  /* STB_GLOBAL, STB_WEAK */
            
            if (sym.st_name >= strtab_sh.sh_size) continue;
            const char* name = strtab_buf + sym.st_name;
            if (!name || !*name) continue;
            g_symtab[n++] = { name, sym.st_value };
        }

        i += chunk;
    }
    trace_printf("[symtab] done, parsed %u symbols\n", (unsigned)n);
    return n;
}

firmware::link_context g_ctx;
firmware::dynamic_linker g_linker(g_ctx);
const char* g_last_error = nullptr;

struct symtab_init {
    symtab_init() noexcept {
        std::size_t n = 0;
        if (auto* flash = flash_symtab(n)) {
            trace_printf("[symtab] %u symbols from flash\n", (unsigned)n);
            g_ctx.set_symtab(flash, n);
            return;
        }
        trace_puts("[symtab] no flash table, falling back to ELF over semihosting\n");
        n = load_symtab();
        if (n > 0) g_ctx.set_symtab(g_symtab, n);
    }
};
static symtab_init g_symtab_init;  /* runs before main() */

} // namespace

namespace dl {

handle dlopen(const char* path, Mode) noexcept {
    auto result = firmware::load_semihost(g_linker, path);
    if (!result) {
        switch (result.err()) {
        case loader::error::FILE_NOT_FOUND:    g_last_error = "FileNotFound"; break;
        case loader::error::SHORT_READ:       g_last_error = "ShortRead"; break;
        case loader::error::NOT_ELF:          g_last_error = "NotElf"; break;
        case loader::error::NOT_ARM32:        g_last_error = "NotArm32"; break;
        case loader::error::NOT_SHARED_OBJECT: g_last_error = "NotSharedObject"; break;
        case loader::error::BAD_SECTION:      g_last_error = "BadSection"; break;
        case loader::error::SYMBOL_NOT_FOUND:  g_last_error = "SymbolNotFound"; break;
        case loader::error::UNSUPPORTED_RELOC:g_last_error = "UnsupportedReloc"; break;
        case loader::error::OUT_OF_MEMORY:     g_last_error = "OutOfMemory"; break;
        default:                             g_last_error = "UnknownError"; break;
        }
        return nullptr;
    }
    g_last_error = nullptr;
    return *result;
}

void* dlsym(handle h, const char* name) noexcept {
    if (!h) { g_last_error = "dlsym: null handle"; return nullptr; }
    auto result = g_linker.lookup(h, name);
    if (!result) {
        g_last_error = "dlsym: symbol not found";
        return nullptr;
    }
    g_last_error = nullptr;
    return *result;
}

int dlclose(handle h) noexcept {
    if (!h) { g_last_error = "dlclose: null handle"; return -1; }
    g_linker.unload(h);
    g_last_error = nullptr;
    return 0;
}

const char* dlerror() noexcept {
    auto* e = g_last_error;
    g_last_error = nullptr;
    return e;
}

} // namespace dl
