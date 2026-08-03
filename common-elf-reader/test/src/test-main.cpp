/**
 * @file test-main.cpp
 * @brief Host test exercising the common-elf-reader on 411ce-100Mhz.elf.
 *
 * Tests: fd_byte_source, elf_image::parse, header, program/section headers,
 *        find_section, image_size, image_base, source().read_at,
 *        collect_relocations (SCL_HOSTED).
 */
#include "loader/elf-image.hpp"
#include "loader/sources/fd-byte-source.hpp"
#include "loader/detail/bytes.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, ...)                                                    \
    do {                                                                    \
        if (expr) { ++g_pass; }                                             \
        else { ++g_fail; std::fprintf(stderr, "FAIL %s:%d: " __VA_ARGS__,  \
                                      __FILE__, __LINE__); std::fputc('\n', stderr); } \
    } while (0)

// ── 1. byte_source: open ─────────────────────────────────────────────────

static void test_open(loader::byte_source& src) {
    CHECK(src.size() > 0, "file size must be > 0 (got %zu)", src.size());

    std::byte magic[4]{};
    auto r = src.read_at(0, magic);
    CHECK(r, "read_at(0, 4) must succeed");
    CHECK(magic[0] == std::byte{0x7f} &&
          magic[1] == std::byte{'E'} &&
          magic[2] == std::byte{'L'} &&
          magic[3] == std::byte{'F'}, "magic must be 0x7f ELF");

    // out-of-range read
    std::byte one{};
    auto rr = src.read_at(src.size() + 1, {&one, 1});
    CHECK(!rr, "read past EOF must fail");
    CHECK(rr.err() == loader::error::SHORT_READ, "error must be SHORT_READ");
}

// ── 2. parse: valid ARM ELF32 ────────────────────────────────────────────

static void test_parse(loader::elf_image& img) {
    const auto& h = img.header();

    CHECK(h.e_ident[0] == 0x7f && h.e_ident[1] == 'E' &&
          h.e_ident[2] == 'L'  && h.e_ident[3] == 'F', "magic in header");

    CHECK(h.e_ident[4] == 1, "ELFCLASS32");
    CHECK(h.e_ident[5] == 1, "ELFDATA2LSB");
    CHECK(h.e_machine == 40, "EM_ARM (40)");
    CHECK(h.e_type == loader::elf::ET_EXEC, "ET_EXEC (2)");
    CHECK(h.e_ehsize == 52, "Ehdr size 52");
    CHECK(h.e_phentsize == 32, "Phdr size 32");
    CHECK(h.e_shentsize == 40, "Shdr size 40");
    CHECK(h.e_phnum == 4, "4 program headers");
    CHECK(h.e_shnum == 24, "24 section headers");
    CHECK(h.e_shstrndx == 23, "shstrtab at index 23");
    CHECK(h.e_entry == 0x08000199u, "entry 0x08000199");
}

// ── 3. program headers ───────────────────────────────────────────────────

static void test_program_headers(loader::elf_image& img) {
    const auto& h = img.header();
    int load_count = 0;

    for (loader::elf::half i = 0; i < h.e_phnum; ++i) {
        loader::elf::Phdr ph{};
        auto r = img.program_header(i, ph);
        CHECK(r, "program_header(%u) must succeed", i);

        if (ph.p_type == loader::elf::PT_LOAD) {
            ++load_count;
            CHECK(ph.p_memsz >= ph.p_filesz,
                  "PT_LOAD memsz (%u) >= filesz (%u)", ph.p_memsz, ph.p_filesz);
        }
    }
    CHECK(load_count == 3, "expected 3 PT_LOAD segments, got %d", load_count);

    // out-of-range
    loader::elf::Phdr bad{};
    CHECK(!img.program_header(h.e_phnum, bad),
          "program_header(oob) must fail");
}

// ── 4. section headers ───────────────────────────────────────────────────

static void test_section_headers(loader::elf_image& img) {
    const auto& h = img.header();
    int null_count = 0;

    for (loader::elf::half i = 0; i < h.e_shnum; ++i) {
        loader::elf::Shdr sh{};
        auto r = img.section_header(i, sh);
        CHECK(r, "section_header(%u) must succeed", i);
        if (sh.sh_type == 0) ++null_count; // SHT_NULL
    }
    CHECK(null_count >= 1, "at least 1 SHT_NULL section");

    // out-of-range
    loader::elf::Shdr bad{};
    CHECK(!img.section_header(h.e_shnum, bad),
          "section_header(oob) must fail");
}

// ── 5. find_section by name ──────────────────────────────────────────────

static void test_find_section(loader::elf_image& img) {
    // These sections are present in the .elf fixture.
    for (const char* name : {".text", ".data", ".bss", ".isr_vector",
                             ".ARM.exidx", ".comment", ".shstrtab"}) {
        auto r = img.find_section(name);
        CHECK(r, "find_section(\"%s\") must succeed", name);
    }

    // Non-existent section
    auto bad = img.find_section(".no_such_section");
    CHECK(!bad, "find_section(missing) must fail");
    CHECK(bad.err() == loader::error::BAD_SECTION, "error must be BAD_SECTION");

    // Verify .text properties
    auto text = img.find_section(".text");
    CHECK(text->sh_addr == 0x08000440u, ".text addr 0x08000440");
    CHECK(text->sh_size > 0, ".text size > 0");
}

// ── 6. image_size / image_base ───────────────────────────────────────────

static void test_image_size_base(loader::elf_image& img) {
    auto sz = img.image_size();
    CHECK(sz, "image_size must succeed");
    // From readelf: highest PT_LOAD end = 0x20000070 + 0x4b8 = 0x20000528
    CHECK(*sz == 0x20000528u, "image_size 0x%x (expected 0x20000528)", *sz);

    auto base = img.image_base();
    CHECK(base, "image_base must succeed");
    CHECK(*base == 0x08000000u, "image_base 0x%x (expected 0x08000000)", *base);
}

// ── 7. source().read_at: read raw section content ────────────────────────

static void test_read_section_content(loader::elf_image& img) {
    auto comment = img.find_section(".comment");
    CHECK(comment, ".comment section must exist");

    std::byte buf[64]{};
    auto r = img.source().read_at(comment->sh_offset,
                                  {buf, comment->sh_size < 64 ? comment->sh_size : 64});
    CHECK(r, "read_at(.comment) must succeed");

    // .comment typically starts with GCC version string
    CHECK(buf[0] != std::byte{0}, ".comment content must be non-zero");
}

// ── 8. detail::bytes helpers ─────────────────────────────────────────────

static void test_detail_bytes() {
    std::byte buf[8]{};
    // store_word then load_word round-trip
    loader::detail::store_word({buf, 8}, 0, 0xDEADBEEFu);
    CHECK(loader::detail::load_word({buf, 8}, 0) == 0xDEADBEEFu,
          "store/load round-trip");

    // bounds: load_word out-of-range returns 0
    CHECK(loader::detail::load_word({buf, 8}, 8) == 0,
          "load_word oob returns 0");

    // bounds: store_word out-of-range is no-op
    loader::detail::store_word({buf, 8}, 8, 0x12345678u);
    CHECK(loader::detail::load_word({buf, 8}, 0) == 0xDEADBEEFu,
          "store_word oob is no-op");

    // load_byte
    buf[3] = std::byte{0x42};
    CHECK(loader::detail::load_byte({buf, 8}, 3) == 0x42, "load_byte");
    CHECK(loader::detail::load_byte({buf, 8}, 8) == 0, "load_byte oob");

    // name_at
    const char* names = "foo\0bar\0baz";
    auto span = std::as_bytes(std::span{names, 12});
    auto s = loader::detail::name_at(span, 4);
    CHECK(s == "bar", "name_at(4) == \"bar\"");
    auto empty = loader::detail::name_at(span, 99);
    CHECK(empty.empty(), "name_at(oob) is empty");
}

// ── 9. collect_relocations (SCL_HOSTED) ──────────────────────────────────

#if defined(SCL_HOSTED)
static void test_collect_relocations(loader::elf_image& img) {
    auto rels = img.collect_relocations();
    // The .elf fixture has .rel.dyn; may be empty if no relocations, but
    // the call itself must succeed.
    CHECK(rels, "collect_relocations must succeed");
}
#endif

// ── main ─────────────────────────────────────────────────────────────────

int main() {
    std::puts("=== common-elf-reader test ===");

    // 1. Open
    auto src = loader::fd_byte_source::open_file("411ce-100Mhz.elf");
    if (!src) {
        std::fprintf(stderr, "cannot open 411ce-100Mhz.elf\n");
        return 1;
    }
    test_open(*src);

    // 2. Parse
    auto img = loader::elf_image::parse(*src);
    if (!img) {
        std::fprintf(stderr, "parse failed: %d\n", static_cast<int>(img.err()));
        return 1;
    }
    test_parse(*img);
    test_program_headers(*img);
    test_section_headers(*img);
    test_find_section(*img);
    test_image_size_base(*img);
    test_read_section_content(*img);
    test_detail_bytes();
#if defined(SCL_HOSTED)
    test_collect_relocations(*img);
#endif

    std::printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
