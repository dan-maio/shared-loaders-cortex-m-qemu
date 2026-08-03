/**
 * @file test-main.cpp
 * @brief Bare-metal ARM test for common-elf-reader on WeAct STM32F411CE.
 *
 * Uses the WeAct platform's semihosting I/O (BKPT 0xAB) for file access
 * and trace output. No libc, no newlib, no POSIX.
 */
#include "loader/elf-image.hpp"
#include "loader/byte-source.hpp"
#include "loader/error.hpp"
#include "loader/detail/bytes.hpp"

#include "semihost.h"
#include "board.h"

#include <cstddef>
#include <cstring>
#include <span>

// C++ runtime: sized delete needed by virtual dtors with -nostdlib.
void operator delete(void*, unsigned int) noexcept {}
void operator delete(void* p) noexcept { (void)p; }

// ── semihost byte_source ─────────────────────────────────────────────────

class semihost_byte_source final : public loader::byte_source {
public:
    semihost_byte_source() noexcept = default;

    ~semihost_byte_source() override {
        if (fd_ >= 0) semihost_close(fd_);
    }

    static loader::result<semihost_byte_source> open_file(const char* path) noexcept {
        int fd = semihost_open(path, 1);
        if (fd < 0) return loader::unexpected(loader::error::FILE_NOT_FOUND);
        long sz = semihost_flen(fd);
        if (sz < 0) { semihost_close(fd); return loader::unexpected(loader::error::SHORT_READ); }
        semihost_byte_source src;
        src.fd_ = fd;
        src.size_ = static_cast<std::size_t>(sz);
        return src;
    }

    semihost_byte_source(semihost_byte_source&& o) noexcept : fd_(o.fd_), size_(o.size_) {
        o.fd_ = -1; o.size_ = 0;
    }
    semihost_byte_source& operator=(semihost_byte_source&& o) noexcept {
        if (this != &o) { if (fd_ >= 0) semihost_close(fd_); fd_ = o.fd_; size_ = o.size_; o.fd_ = -1; o.size_ = 0; }
        return *this;
    }

    [[nodiscard]] std::size_t size() const noexcept override { return size_; }

    [[nodiscard]] loader::result<void>
    read_at(std::size_t offset, std::span<std::byte> out) noexcept override {
        if (fd_ < 0) return loader::unexpected(loader::error::FILE_NOT_FOUND);
        if (offset > size_ || out.size() > size_ - offset)
            return loader::unexpected(loader::error::SHORT_READ);
        if (semihost_seek(fd_, static_cast<long>(offset)) != 0)
            return loader::unexpected(loader::error::SHORT_READ);
        int rem = semihost_read(fd_, out.data(), static_cast<int>(out.size()));
        if (rem != 0) return loader::unexpected(loader::error::SHORT_READ);
        return {};
    }

private:
    int fd_{-1};
    std::size_t size_{0};
};

// ── trace helpers ────────────────────────────────────────────────────────

static void put_uint(unsigned v, unsigned base) {
    char b[12]; int i = 0;
    if (v == 0) { trace_putc('0'); return; }
    static const char hx[] = "0123456789abcdef";
    while (v) { b[i++] = hx[v % base]; v /= base; }
    while (i) trace_putc(b[--i]);
}

static int g_pass = 0;
static int g_fail = 0;
static int g_group_pass = 0;
static int g_group_fail = 0;

#define CHECK(expr, label)                                                  \
    do {                                                                    \
        if (expr) { ++g_pass; ++g_group_pass; }                             \
        else { ++g_fail; ++g_group_fail;                                    \
               trace_puts("  FAIL "); trace_puts(label); trace_putc('\n'); } \
    } while (0)

static void group_begin(const char* name) {
    trace_puts("[TEST] ");
    trace_puts(name);
    trace_putc('\n');
    g_group_pass = 0;
    g_group_fail = 0;
}

static void group_end() {
    trace_puts("  -> ");
    put_uint(g_group_pass, 10);
    trace_puts(" ok");
    if (g_group_fail) {
        trace_puts(", ");
        put_uint(g_group_fail, 10);
        trace_puts(" FAIL");
    }
    trace_putc('\n');
}

// ── test groups ──────────────────────────────────────────────────────────

static void test_open(loader::byte_source& src) {
    group_begin("1. byte_source: open file, read, bounds");
    CHECK(src.size() > 0, "size>0");

    std::byte magic[4]{};
    auto r = src.read_at(0, magic);
    CHECK(r, "read_at(0,4)");
    CHECK(magic[0] == std::byte{0x7f} && magic[1] == std::byte{'E'} &&
          magic[2] == std::byte{'L'} && magic[3] == std::byte{'F'}, "magic=0x7fELF");

    std::byte one{};
    auto rr = src.read_at(src.size() + 1, {&one, 1});
    CHECK(!rr, "read-past-EOF-fails");
    CHECK(rr.err() == loader::error::SHORT_READ, "err=SHORT_READ");
    group_end();
}

static void test_parse(loader::elf_image& img) {
    group_begin("2. elf_image::parse: validate Ehdr fields");
    const auto& h = img.header();
    CHECK(h.e_ident[0]==0x7f && h.e_ident[1]=='E' && h.e_ident[2]=='L' && h.e_ident[3]=='F', "magic");
    CHECK(h.e_ident[4] == 1, "ELFCLASS32");
    CHECK(h.e_ident[5] == 1, "ELFDATA2LSB");
    CHECK(h.e_machine == 40, "EM_ARM=40");
    CHECK(h.e_type == loader::elf::ET_EXEC, "ET_EXEC");
    CHECK(h.e_ehsize == 52, "Ehdr=52bytes");
    CHECK(h.e_phnum == 4, "4 program headers");
    CHECK(h.e_shnum > 0, "section headers > 0");
    CHECK(h.e_shstrndx < h.e_shnum, "shstrndx valid");
    CHECK(h.e_entry == 0x08000199u, "entry=0x08000199");
    group_end();
}

static void test_program_headers(loader::elf_image& img) {
    group_begin("3. program_header: iterate PT_LOAD segments");
    const auto& h = img.header();
    int load_count = 0;
    for (loader::elf::half i = 0; i < h.e_phnum; ++i) {
        loader::elf::Phdr ph{};
        auto r = img.program_header(i, ph);
        CHECK(r, "phdr-read");
        if (ph.p_type == loader::elf::PT_LOAD) {
            ++load_count;
            CHECK(ph.p_memsz >= ph.p_filesz, "memsz>=filesz");
        }
    }
    CHECK(load_count == 3, "3 PT_LOAD segments");
    loader::elf::Phdr bad{};
    CHECK(!img.program_header(h.e_phnum, bad), "oob-returns-error");
    group_end();
}

static void test_section_headers(loader::elf_image& img) {
    group_begin("4. section_header: iterate all, bounds check");
    const auto& h = img.header();
    int null_count = 0;
    for (loader::elf::half i = 0; i < h.e_shnum; ++i) {
        loader::elf::Shdr sh{};
        CHECK(img.section_header(i, sh), "shdr-read");
        if (sh.sh_type == 0) ++null_count;
    }
    CHECK(null_count >= 1, "has SHT_NULL");
    loader::elf::Shdr bad{};
    CHECK(!img.section_header(h.e_shnum, bad), "oob-returns-error");
    group_end();
}

static void test_find_section(loader::elf_image& img) {
    group_begin("5. find_section: lookup by name");
    for (const char* name : {".text", ".data", ".bss", ".isr_vector", ".ARM.exidx", ".shstrtab"}) {
        CHECK(img.find_section(name), name);
    }
    CHECK(!img.find_section(".nope"), "missing->BAD_SECTION");
    CHECK(img.find_section(".nope").err() == loader::error::BAD_SECTION, "err=BAD_SECTION");
    auto text = img.find_section(".text");
    CHECK(text->sh_addr == 0x08000440u, ".text@0x08000440");
    CHECK(text->sh_size > 0, ".text size > 0");
    group_end();
}

static void test_image_size_base(loader::elf_image& img) {
    group_begin("6. image_size / image_base from PT_LOAD");
    auto sz = img.image_size();
    CHECK(sz, "image_size ok");
    CHECK(*sz == 0x20000528u, "size=0x20000528");
    auto base = img.image_base();
    CHECK(base, "image_base ok");
    CHECK(*base == 0x08000000u, "base=0x08000000");
    group_end();
}

static void test_read_section_content(loader::elf_image& img) {
    group_begin("7. source().read_at: read raw section bytes");
    auto comment = img.find_section(".comment");
    CHECK(comment, ".comment exists");
    std::byte buf[64]{};
    auto r = img.source().read_at(comment->sh_offset,
                                  {buf, comment->sh_size < 64 ? comment->sh_size : 64});
    CHECK(r, ".comment read ok");
    CHECK(buf[0] != std::byte{0}, ".comment non-empty");
    group_end();
}

static void test_detail_bytes() {
    group_begin("8. detail::bytes: load/store/name_at helpers");
    std::byte buf[8]{};
    loader::detail::store_word({buf, 8}, 0, 0xDEADBEEFu);
    CHECK(loader::detail::load_word({buf, 8}, 0) == 0xDEADBEEFu, "store+load roundtrip");
    CHECK(loader::detail::load_word({buf, 8}, 8) == 0, "load_word oob=0");
    loader::detail::store_word({buf, 8}, 8, 0x12345678u);
    CHECK(loader::detail::load_word({buf, 8}, 0) == 0xDEADBEEFu, "store_word oob=noop");
    buf[3] = std::byte{0x42};
    CHECK(loader::detail::load_byte({buf, 8}, 3) == 0x42, "load_byte ok");
    CHECK(loader::detail::load_byte({buf, 8}, 8) == 0, "load_byte oob=0");
    const char* names = "foo\0bar\0baz";
    auto span = std::as_bytes(std::span{names, 12});
    CHECK(loader::detail::name_at(span, 4) == "bar", "name_at(4)=bar");
    CHECK(loader::detail::name_at(span, 99).empty(), "name_at oob=empty");
    group_end();
}

// ── main ─────────────────────────────────────────────────────────────────

int main() {
    board_init();
    trace_puts("=== common-elf-reader test-arm (WeAct STM32F411CE) ===\n");

    // Read ELF filename from semihosting command line.
    // cmdline = "elf-reader-test-arm 411ce-100Mhz.elf" — skip argv[0], use argv[1].
    char cmdline[128];
    if (semihost_get_cmdline(cmdline, sizeof(cmdline)) < 0 || cmdline[0] == '\0') {
        trace_puts("ERROR: no ELF filename on cmdline\n");
        return 1;
    }
    const char* elf_path = cmdline;
    while (*elf_path && *elf_path != ' ') ++elf_path;
    while (*elf_path == ' ')        ++elf_path;
    if (*elf_path == '\0') {
        trace_puts("ERROR: no ELF path in cmdline\n");
        return 1;
    }
    trace_puts("fixture: ");
    trace_puts(elf_path);
    trace_putc('\n');

    auto src_r = semihost_byte_source::open_file(elf_path);
    if (!src_r) {
        trace_puts("ERROR: cannot open ");
        trace_puts(elf_path);
        trace_putc('\n');
        return 1;
    }
    test_open(*src_r);

    auto img = loader::elf_image::parse(*src_r);
    if (!img) {
        trace_puts("ERROR: parse failed\n");
        return 1;
    }
    test_parse(*img);
    test_program_headers(*img);
    test_section_headers(*img);
    test_find_section(*img);
    test_image_size_base(*img);
    test_read_section_content(*img);
    test_detail_bytes();

    trace_puts("\n=== ");
    put_uint(g_pass, 10);
    trace_puts(" passed, ");
    put_uint(g_fail, 10);
    trace_puts(" failed ===\n");

    return g_fail ? 1 : 0;
}
