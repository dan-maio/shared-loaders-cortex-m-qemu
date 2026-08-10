/**
 * @file elf2json.cpp
 * @brief Export an ARM ELF32 binary as JSON for the AetherELF GUI Visualizer.
 *
 * Uses common-elf-reader to parse the ELF and outputs a JSON structure
 * compatible with the GUI's expected format:
 *   { header, program_headers, section_headers, symbols, dynamic, plt }
 *
 * Usage: elf2json <input.elf> [output.json]
 *        If no output file is given, writes to stdout.
 */
#include "loader/elf-image.hpp"
#include "loader/sources/fd-byte-source.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <cstring>
#include <iomanip>

// ---- JSON escaping helpers --------------------------------------------------

static std::string json_escape(std::string_view sv) {
    std::string out;
    out.reserve(sv.size() + 8);
    out += '"';
    for (char c : sv) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

// ---- Read raw bytes from the byte_source ------------------------------------

static std::vector<std::byte> read_bytes(loader::byte_source& src,
                                          std::size_t offset, std::size_t size) {
    std::vector<std::byte> buf(size);
    if (size == 0) return buf;
    auto res = src.read_at(offset, {buf.data(), buf.size()});
    if (!res) {
        std::cerr << "Warning: short read at offset 0x" << std::hex << offset
                  << " size " << std::dec << size << "\n";
        buf.clear();
    }
    return buf;
}

// ---- Read a null-terminated string from a string table ----------------------

static std::string read_string(const std::vector<char>& strtab, std::size_t offset) {
    if (offset >= strtab.size()) return "";
    const char* start = strtab.data() + offset;
    const char* end = strtab.data() + strtab.size();
    const char* p = start;
    while (p < end && *p != '\0') ++p;
    return std::string(start, p);
}

// ---- Main export ------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <input.elf> [output.json]\n";
        return 1;
    }

    const char* elf_path = argv[1];
    const char* json_path = (argc >= 3) ? argv[2] : nullptr;

    // 1. Open ELF file
    auto src_res = loader::fd_byte_source::open_file(elf_path);
    if (!src_res) {
        std::cerr << "Error: Cannot open " << elf_path << "\n";
        return 1;
    }
    auto& src = *src_res;

    // 2. Parse ELF image
    auto img_res = loader::elf_image::parse(src);
    if (!img_res) {
        std::cerr << "Error: Not a valid ARM ELF32 binary\n";
        return 1;
    }
    const auto& img = *img_res;
    const auto& eh = img.header();

    // 3. Collect section headers and build string table
    std::vector<loader::elf::Shdr> sections(eh.e_shnum);
    std::vector<char> shstrtab;

    for (std::size_t i = 0; i < eh.e_shnum; ++i) {
        auto r = img.section_header(static_cast<loader::elf::half>(i), sections[i]);
        if (!r) {
            std::cerr << "Warning: cannot read section header " << i << "\n";
        }
    }

    // Read the section header string table
    if (eh.e_shstrndx < eh.e_shnum) {
        const auto& str_sec = sections[eh.e_shstrndx];
        shstrtab.resize(str_sec.sh_size);
        auto rr = src.read_at(str_sec.sh_offset,
                              {reinterpret_cast<std::byte*>(shstrtab.data()), shstrtab.size()});
        if (!rr) {
            std::cerr << "Warning: cannot read section string table\n";
            shstrtab.clear();
        }
    }

    // Helper: section name
    auto sec_name = [&](std::size_t idx) -> std::string {
        if (idx >= sections.size()) return "";
        return read_string(shstrtab, sections[idx].sh_name);
    };

    // 4. Find .symtab, .strtab, .dynsym, .dynstr, .rel.plt, .rel.dyn
    const loader::elf::Shdr* symtab_sec = nullptr;
    const loader::elf::Shdr* strtab_sec = nullptr;
    const loader::elf::Shdr* dynsym_sec = nullptr;
    const loader::elf::Shdr* dynstr_sec = nullptr;
    std::vector<const loader::elf::Shdr*> rel_secs;

    for (std::size_t i = 0; i < eh.e_shnum; ++i) {
        std::string name = sec_name(i);
        if (name == ".symtab")  symtab_sec = &sections[i];
        if (name == ".strtab")  strtab_sec = &sections[i];
        if (name == ".dynsym")  dynsym_sec = &sections[i];
        if (name == ".dynstr")  dynstr_sec = &sections[i];
        if (name.starts_with(".rel.") || name.starts_with(".rela."))
            rel_secs.push_back(&sections[i]);
    }

    // 5. Read symbol tables (.symtab and .dynsym)
    struct SymEntry {
        std::string name;
        uint32_t value;
        uint32_t size;
        uint8_t  bind;
        uint8_t  type;
        uint16_t shndx;
        std::string source;
    };
    std::vector<SymEntry> all_syms;

    auto read_symtab = [&](const loader::elf::Shdr* sec,
                           const loader::elf::Shdr* str_sec,
                           const char* source) {
        if (!sec || !str_sec) return;
        std::vector<char> strtab(str_sec->sh_size);
        auto rr = src.read_at(str_sec->sh_offset,
                              {reinterpret_cast<std::byte*>(strtab.data()), strtab.size()});
        if (!rr) return;

        std::size_t count = sec->sh_size / sizeof(loader::elf::Sym);
        std::vector<loader::elf::Sym> syms(count);
        rr = src.read_at(sec->sh_offset,
                         {reinterpret_cast<std::byte*>(syms.data()), sec->sh_size});
        if (!rr) return;

        for (std::size_t i = 0; i < count; ++i) {
            const auto& s = syms[i];
            SymEntry e;
            e.name = read_string(strtab, s.st_name);
            e.value = s.st_value;
            e.size = s.st_size;
            e.bind = loader::elf::st_bind(s.st_info);
            e.type = loader::elf::st_type(s.st_info);
            e.shndx = s.st_shndx;
            e.source = source;
            all_syms.push_back(std::move(e));
        }
    };

    read_symtab(symtab_sec, strtab_sec, ".symtab");
    read_symtab(dynsym_sec, dynstr_sec, ".dynsym");

    // 6. Read dynamic entries (.dynamic section)
    struct DynEntry { int32_t tag; uint32_t val; };
    std::vector<DynEntry> dynamics;
    for (std::size_t i = 0; i < eh.e_shnum; ++i) {
        if (sec_name(i) == ".dynamic") {
            const auto& ds = sections[i];
            std::size_t count = ds.sh_size / sizeof(loader::elf::Dyn);
            std::vector<loader::elf::Dyn> dyns(count);
            auto rr = src.read_at(ds.sh_offset,
                                  {reinterpret_cast<std::byte*>(dyns.data()), ds.sh_size});
            if (rr) {
                for (std::size_t j = 0; j < count; ++j) {
                    if (dyns[j].d_tag == loader::elf::DT_NULL) break;
                    dynamics.push_back({dyns[j].d_tag, dyns[j].d_val});
                }
            }
            break;
        }
    }

    // 7. Collect relocations (PLT and dynamic)
    struct RelEntry { uint32_t offset; uint32_t info; std::string section; };
    std::vector<RelEntry> relocations;
    for (const auto* rs : rel_secs) {
        std::size_t count = rs->sh_size / sizeof(loader::elf::Rel);
        std::vector<loader::elf::Rel> rels(count);
        auto rr = src.read_at(rs->sh_offset,
                              {reinterpret_cast<std::byte*>(rels.data()), rs->sh_size});
        if (!rr) continue;
        std::string rname = sec_name(rs - &sections[0]);
        for (std::size_t j = 0; j < count; ++j) {
            relocations.push_back({rels[j].r_offset, rels[j].r_info, rname});
        }
    }

    // ---- Build JSON output --------------------------------------------------
    std::ostringstream os;
    os << "{\n";

    // header
    os << "  \"header\": {\n";
    os << "    \"type\": "    << eh.e_type    << ",\n";
    os << "    \"machine\": " << eh.e_machine << ",\n";
    os << "    \"entry\": "   << eh.e_entry   << ",\n";
    os << "    \"phoff\": "   << eh.e_phoff   << ",\n";
    os << "    \"shoff\": "   << eh.e_shoff   << ",\n";
    os << "    \"phnum\": "   << eh.e_phnum   << ",\n";
    os << "    \"shnum\": "   << eh.e_shnum   << "\n";
    os << "  },\n";

    // program_headers
    os << "  \"program_headers\": [\n";
    for (std::size_t i = 0; i < eh.e_phnum; ++i) {
        loader::elf::Phdr ph;
        auto phr = img.program_header(static_cast<loader::elf::half>(i), ph);
        if (!phr) {
            std::cerr << "Warning: cannot read program header " << i << "\n";
            continue;
        }
        os << "    {";
        os << "\"type\":"   << ph.p_type   << ",";
        os << "\"offset\":" << ph.p_offset << ",";
        os << "\"vaddr\":"  << ph.p_vaddr  << ",";
        os << "\"paddr\":"  << ph.p_paddr  << ",";
        os << "\"filesz\":" << ph.p_filesz << ",";
        os << "\"memsz\":"  << ph.p_memsz  << ",";
        os << "\"flags\":"  << ph.p_flags  << ",";
        os << "\"align\":"  << ph.p_align;
        os << "}";
        if (i + 1 < eh.e_phnum) os << ",";
        os << "\n";
    }
    os << "  ],\n";

    // section_headers
    os << "  \"section_headers\": [\n";
    for (std::size_t i = 0; i < eh.e_shnum; ++i) {
        const auto& sh = sections[i];
        os << "    {";
        os << "\"index\":"   << i << ",";
        os << "\"name\":"    << json_escape(sec_name(i)) << ",";
        os << "\"type\":"    << sh.sh_type    << ",";
        os << "\"flags\":"   << sh.sh_flags   << ",";
        os << "\"addr\":"    << sh.sh_addr    << ",";
        os << "\"offset\":"  << sh.sh_offset  << ",";
        os << "\"size\":"    << sh.sh_size    << ",";
        os << "\"link\":"    << sh.sh_link    << ",";
        os << "\"info\":"    << sh.sh_info    << ",";
        os << "\"entsize\":" << sh.sh_entsize;
        os << "}";
        if (i + 1 < eh.e_shnum) os << ",";
        os << "\n";
    }
    os << "  ],\n";

    // dynamic
    os << "  \"dynamic\": [\n";
    for (std::size_t i = 0; i < dynamics.size(); ++i) {
        os << "    {\"tag\":" << dynamics[i].tag << ",\"val\":" << dynamics[i].val << "}";
        if (i + 1 < dynamics.size()) os << ",";
        os << "\n";
    }
    os << "  ],\n";

    // plt (relocations used as PLT entries)
    os << "  \"plt\": [\n";
    {
        std::size_t written = 0;
        for (const auto& r : relocations) {
            if (r.section.find("plt") != std::string::npos) {
                if (written > 0) os << ",\n";
                os << "    {\"offset\":" << r.offset
                   << ",\"info\":" << r.info
                   << ",\"section\":" << json_escape(r.section) << "}";
                ++written;
            }
        }
        if (written > 0) os << "\n";
    }
    os << "  ],\n";

    // symbols
    os << "  \"symbols\": [\n";
    for (std::size_t i = 0; i < all_syms.size(); ++i) {
        const auto& s = all_syms[i];
        os << "    {";
        os << "\"name\":"   << json_escape(s.name) << ",";
        os << "\"value\":"  << s.value << ",";
        os << "\"size\":"   << s.size  << ",";
        os << "\"bind\":"   << static_cast<int>(s.bind) << ",";
        os << "\"type\":"   << static_cast<int>(s.type) << ",";
        os << "\"shndx\":"  << s.shndx << ",";
        os << "\"source\":" << json_escape(s.source);
        os << "}";
        if (i + 1 < all_syms.size()) os << ",";
        os << "\n";
    }
    os << "  ]\n";

    os << "}\n";

    // ---- Write output -------------------------------------------------------
    if (json_path) {
        std::ofstream ofs(json_path);
        if (!ofs) {
            std::cerr << "Error: cannot write to " << json_path << "\n";
            return 1;
        }
        ofs << os.str();
        std::cout << "Exported " << elf_path << " -> " << json_path << "\n";
    } else {
        std::cout << os.str();
    }

    return 0;
}
