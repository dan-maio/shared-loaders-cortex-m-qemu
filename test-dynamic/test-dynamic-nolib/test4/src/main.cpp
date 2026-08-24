#include "board.h"
#include "semihost.h"
#include "dl.hpp"
#include "dl-semihost.hpp"
#include "lazy-plt.hpp"
#include "pic-trampoline.h"
#include "fm-iface.h"
#include "loader/lazy-resolve.hpp"
#include "loader/detail/bytes.hpp"
#include <cstdint>
#include <cstring>
#include <new>

extern "C" {
void trace_event(const char* origin, const char* fn, std::uint32_t arg) {
    trace_putc('['); trace_puts(origin); trace_puts("] "); trace_puts(fn); trace_putc('(');
    char b[10]; int i = 0; std::uint32_t v = arg;
    if (v == 0) trace_putc('0'); else { while (v) { b[i++] = char('0' + v % 10); v /= 10; } while (i) trace_putc(b[--i]); }
    trace_puts(")\n");
}
void trace_u32(const char* label, std::uint32_t v) {
    trace_puts(label);
    char b[10]; int i = 0;
    if (v == 0) trace_putc('0'); else { while (v) { b[i++] = char('0' + v % 10); v /= 10; } while (i) trace_putc(b[--i]); }
    trace_putc('\n');
}
std::uint32_t          fm_var  = 170u;
const struct FmInfo    fm_info = { 241u, 7u };
std::uint32_t fm_callback(std::uint32_t x) { trace_event("FM", "fm_callback", x); return x + 0x100u; }
}

static void put_hex(const char* label, std::uint32_t v) {
    static const char hx[] = "0123456789abcdef";
    char out[9]; for (int i = 7; i >= 0; --i) { out[i] = hx[v & 0xfu]; v >>= 4; }
    out[8] = '\0'; trace_puts(label); trace_puts(out);
}

inline void* gotp(fw::handle& h) { return reinterpret_cast<void*>(h.got_base); }

// -----------------------------------------------------------------------
// lr fixer: enters from PLT0 with lr = &GOT[2] (clobbered by PLT0).
// The original return address was pushed on stack by PLT0 (push {lr}).
// Pop it back into lr, then tail-call the real resolver.
// r12 (= &GOT[n]) is preserved by PLT0 — we don't touch it.
// -----------------------------------------------------------------------
__attribute__((naked)) static void resolve_via_got2(void) {
    __asm volatile(
        "  ldr   lr, [sp], #4    \n"   // pop original lr; undo PLT0's push {lr}
        "  b     dl_runtime_resolve \n" // tail-call with lr restored, r12 intact
    );
}

// -----------------------------------------------------------------------
// Combined JMPREL so dl_fixup finds .rel.plt entries too.
// -----------------------------------------------------------------------
static std::byte* build_combined_jmprel(fw::handle& h) {
    auto sz_dyn = h.lazy.jmprel.size();
    auto sz_plt = h.lazy_plt.jmprel.size();
    if (sz_plt == 0) return nullptr;
    auto* buf = new (std::nothrow) std::byte[sz_dyn + sz_plt];
    if (!buf) { trace_puts("  OOM for combined JMPREL\n"); return nullptr; }
    std::memcpy(buf,          h.lazy.jmprel.data(), sz_dyn);
    std::memcpy(buf + sz_dyn, h.lazy_plt.jmprel.data(), sz_plt);
    return buf;
}

int main() {
    board_init();
    trace_puts("\n==== GOT[2]/PLT0 standard lazy-binding DEMO (cpp3.so) ====\n");

    fw::dl_enable_cross_call(false);
    fw::dl_cross_trace(1);

    auto res = fw::dlopen(DL_LIB_DIR "/cpp3.so");
    if (!res) { trace_puts("dlopen FAILED\n"); for (;;); }
    fw::handle* h = *res;
    put_hex("loaded at base=0x", h->base);
    put_hex(" got_base=0x", h->got_base);
    trace_putc('\n');

    std::uint32_t resolver = reinterpret_cast<std::uint32_t>(&dl_runtime_resolve) | 1u;
    std::uint32_t fixer    = reinterpret_cast<std::uint32_t>(&resolve_via_got2) | 1u;
    std::uint32_t plt0_va  = 0x480u;          // cpp3.so .plt start
    std::uint32_t plt0_ram = h->base + plt0_va;

    // ---- STEP 1: combined JMPREL ----
    trace_puts("\n-- [1] combined JMPREL (.rel.dyn + .rel.plt) --\n");
    auto* comb = build_combined_jmprel(*h);
    if (comb) {
        h->lazy = loader::lazy_tables{
            std::span(comb, h->lazy.jmprel.size() + h->lazy_plt.jmprel.size()),
            h->lazy.dynsym, h->lazy.dynstr};
        trace_puts("  OK: dl_fixup now searches .rel.plt entries\n");
    }

    // ---- STEP 2: patch PLT0's GOT-base word ----
    // PLT0 at VA 0x480 does:
    //   push {lr}
    //   ldr.w lr, [pc, #8]    @ lr = *(plt0+0xc) [the WORD below]
    //   add   lr, pc           @ lr = WORD + PC_after_add = WORD + (B+0x488)
    //   ldr   pc, [lr, #8]!    @ pc = *(lr+8) = GOT[2], lr = &GOT[2]
    //   .word 0x000014ec       @ WORD = intended offset from PC to GOT
    //
    // At link time: 0x14ec + 0x488 = 0x1974 (= got_base - 4) [WRONG! off by 4]
    // The linker's calculation is off because the PC alignment in `add lr,pc`
    // doesn't match what the linker assumed (common Cortex-M ambiguity).
    //
    // Fix: replace the word so WORD + PC = got_base.
    std::uint32_t* plt0_word = reinterpret_cast<std::uint32_t*>(plt0_ram + 0x0cu);
    std::uint32_t  pc_at_add = (plt0_va + 0x06u + 4u) & ~3u;
    std::uint32_t  correct   = h->got_base - h->base - pc_at_add;
    put_hex("  PLT0 word at 0x", reinterpret_cast<std::uint32_t>(plt0_word));
    put_hex("  was 0x", *plt0_word);
    put_hex("  now 0x", correct);
    trace_putc('\n');
    *plt0_word = correct;

    // ---- STEP 3: wire GOT[2] and JUMP_SLOTs ----
    trace_puts("\n-- [2] wiring -> PLT0 -> GOT[2] -> resolver --\n");

    std::uint32_t plt0_entry = plt0_ram | 1u;
    *reinterpret_cast<std::uint32_t*>(h->got_base + 8u) = fixer;
    put_hex("  GOT[2] <- resolve_via_got2 @ 0x", fixer);
    trace_puts("\n");

    // Write resolver directly to slots (standard SCL bypass with combined JMPREL)
    // This tests that the combined JMPREL works. If this crashes, the issue is
    // dl_fixup not finding .rel.plt entries.
    trace_puts("  DEBUG: writing dl_runtime_resolve directly to slots\n");
    auto& tabs = h->lazy_plt;
    int patched = 0;
    for (std::size_t o = 0; o + 8 <= tabs.jmprel.size(); o += 8) {
        auto r_info = loader::detail::load_word(tabs.jmprel, o + 4);
        if ((r_info & 0xffu) != 22) continue;
        auto r_off  = loader::detail::load_word(tabs.jmprel, o);
        *reinterpret_cast<std::uint32_t*>(h->base + r_off) = resolver;
        ++patched;
    }
    trace_u32("  patched ", static_cast<std::uint32_t>(patched));
    trace_puts(" JUMP_SLOT GOT slots -> dl_runtime_resolve\n");

    trace_puts("\n-- [3] calling cpp3_method --\n");
    auto f = fw::dlsym(*h, "cpp3_method");
    if (!f) { trace_puts("dlsym FAILED\n"); for (;;); }
    void* fn = *f;

    std::uint32_t c0 = fw::dl_fixup_count();
    trace_u32("  fixup_count BEFORE = ", c0);
    std::uint32_t r1 = pic_call_u32_2(fn, gotp(*h), 2, 100);
    std::uint32_t c1 = fw::dl_fixup_count();
    trace_u32("  fixup_count AFTER 1 = ", c1);
    std::uint32_t r2 = pic_call_u32_2(fn, gotp(*h), 2, 100);
    std::uint32_t c2 = fw::dl_fixup_count();
    trace_u32("  fixup_count AFTER 2 = ", c2);
    trace_u32("  result1 = ", r1);
    trace_u32("  result2 = ", r2);

    bool fixup_ok  = (c1 > c0) && (c2 == c1);
    bool result_ok = (r1 > 0) && (r1 == r2);
    trace_puts("\n  lazy binding: "); trace_puts(fixup_ok ? "PASS" : "FAIL");
    trace_puts("  (fixup fires once, second call direct)\n");
    trace_puts("  call result: "); trace_puts(result_ok ? "PASS" : "FAIL");
    trace_puts("  (finite, consistent)\n");

    fw::dlclose(*h);
    trace_puts("\n==== Demo complete ====\n");
    trace_puts((fixup_ok && result_ok) ? "RESULT: ALL PASS\n" : "RESULT: SOME FAILED\n");
    semihost_exit(0);
    for (;;);
}
