#include "loader/elf-image.hpp"
#include "loader/sources/fd-byte-source.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <set>
#include <cstring>
#include <algorithm>

constexpr uint32_t kMagic = 0x534C4353; // 'SCLS'
constexpr std::string_view kSectionName = ".scl_symtab";

// Check if a symbol name is in the whitelist of allowed internal symbols
bool is_whitelisted(std::string_view name) {
    static const std::string_view whitelist[] = {
        "_impure_ptr", "_Znw", "_Zdl", "__cxa", "__errno",
        "__exidx", "__gnu_Unwind", "__register_frame_info", "__deregister_frame_info"
    };
    for (const auto& w : whitelist) {
        if (name.starts_with(w)) return true;
    }
    return false;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <firmware.elf> <out.bin>\n";
        return 1;
    }

    // 1. Open the ELF file using fd_byte_source
    auto src_res = loader::fd_byte_source::open_file(argv[1]);
    if (!src_res) {
        std::cerr << "Error: Cannot open ELF file " << argv[1] << "\n";
        return 1;
    }
    auto& src = *src_res;

    // 2. Parse the ELF image
    auto img_res = loader::elf_image::parse(src);
    if (!img_res) {
        std::cerr << "Error: Failed to parse ELF image\n";
        return 1;
    }
    const auto& img = *img_res;

    // 3. Find the required sections using elf_image
    auto scl_res = img.find_section(kSectionName);
    auto sym_res = img.find_section(".symtab");
    auto str_res = img.find_section(".strtab");

    if (!scl_res || !sym_res || !str_res) {
        std::cerr << "Error: ELF missing required sections (.scl_symtab, .symtab, or .strtab)\n";
        return 1;
    }
    const auto& scl_sec = *scl_res;
    const auto& sym_sec = *sym_res;
    const auto& str_sec = *str_res;

    // 4. Read the string table
    std::vector<char> strtab(str_sec.sh_size);
    auto str_read = img.source().read_at(str_sec.sh_offset, {reinterpret_cast<std::byte*>(strtab.data()), strtab.size()});
    if (!str_read) {
        std::cerr << "Error: Failed to read string table\n";
        return 1;
    }

    // 5. Read the symbol table
    size_t num_syms = sym_sec.sh_size / sizeof(loader::elf::Sym);
    std::vector<loader::elf::Sym> symtab(num_syms);
    auto sym_read = img.source().read_at(sym_sec.sh_offset, {reinterpret_cast<std::byte*>(symtab.data()), sym_sec.sh_size});
    if (!sym_read) {
        std::cerr << "Error: Failed to read symbol table\n";
        return 1;
    }

    struct Symbol { std::string name; uint32_t addr; };
    std::vector<Symbol> filtered_syms;
    std::set<std::string> seen;

    // 6. Filter dynamic loader symbols (skipping local symbols)
    size_t start_index = sym_sec.sh_info;
    for (size_t i = start_index; i < num_syms; ++i) {
        const auto& sym = symtab[i];
        if (sym.st_value == 0 || sym.st_shndx == 0 || sym.st_shndx >= 0xFFF1) continue;

        // Check binding: Global (1) or Weak (2)
        uint8_t bind = sym.st_info >> 4;
        if (bind != 1 && bind != 2) continue;

        std::string_view name(strtab.data() + sym.st_name);
        if (name.empty()) continue;

        // Pruning logic: filter out internals unless whitelisted
        if (name.starts_with('_') && !is_whitelisted(name)) continue;

        std::string name_str(name);
        if (seen.insert(name_str).second) {
            filtered_syms.push_back({name_str, sym.st_value});
        }
    }

    // 7. Build the binary blob
    std::vector<uint8_t> blob;
    uint32_t count = filtered_syms.size();
    blob.insert(blob.end(), reinterpret_cast<const uint8_t*>(&kMagic), reinterpret_cast<const uint8_t*>(&kMagic) + 4);
    blob.insert(blob.end(), reinterpret_cast<const uint8_t*>(&count), reinterpret_cast<const uint8_t*>(&count) + 4);

    size_t header_sz = 8;
    size_t table_sz = filtered_syms.size() * 8;
    size_t names_offset = header_sz + table_sz;

    std::vector<uint8_t> names_bytes;
    for (const auto& s : filtered_syms) {
        uint32_t name_addr = scl_sec.sh_addr + names_offset + names_bytes.size();
        blob.insert(blob.end(), reinterpret_cast<const uint8_t*>(&name_addr), reinterpret_cast<const uint8_t*>(&name_addr) + 4);
        blob.insert(blob.end(), reinterpret_cast<const uint8_t*>(&s.addr), reinterpret_cast<const uint8_t*>(&s.addr) + 4);

        names_bytes.insert(names_bytes.end(), s.name.begin(), s.name.end());
        names_bytes.push_back('\0');
    }
    blob.insert(blob.end(), names_bytes.begin(), names_bytes.end());

    if (blob.size() > scl_sec.sh_size) {
        std::cerr << "Error: table is " << blob.size() << " bytes but placeholder is only " 
                  << scl_sec.sh_size << " bytes.\n";
        return 1;
    }

    // Pad out to the size of the placeholder section in Flash
    blob.resize(scl_sec.sh_size, 0);

    // Write output binary
    std::ofstream out(argv[2], std::ios::binary);
    out.write(reinterpret_cast<char*>(blob.data()), blob.size());

    std::cout << "gen-symtab: " << count << " symbols, " << (names_offset + names_bytes.size())
              << " bytes used of " << scl_sec.sh_size << " at 0x" << std::hex << scl_sec.sh_addr << std::dec << "\n";
    return 0;
}
