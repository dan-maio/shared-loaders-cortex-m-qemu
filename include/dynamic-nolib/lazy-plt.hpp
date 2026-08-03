/**
 * @file lazy-plt.hpp
 * @brief Phase 5c lazy PLT binding: the runtime resolver entry + fixup, and a
 *        counter exposed so the on-target demo can prove first-call-only binding.
 *
 * Flow: an unbound JUMP_SLOT's GOT slot holds PLT0; the library's GOT[2] holds
 * &dl_runtime_resolve. First call routes PLTn -> PLT0 -> dl_runtime_resolve (which
 * preserves the args), which calls dl_fixup(&GOT[n]) to resolve + patch the slot,
 * then tail-calls the target. Later calls hit the patched slot directly.
 */
#pragma once
#include <cstdint>

extern "C" {
/** @brief Naked Thumb trampoline reached via PLT0/GOT[2]; ip = &GOT[n] on entry. */
void  dl_runtime_resolve(void);
/** @brief Resolve the JUMP_SLOT at GOT slot @p slot, patch it, return the target. */
void* dl_fixup(std::uint32_t* slot) noexcept;
}

namespace fw {
/** @brief Number of lazy fixups performed so far (proves first-call-only binding). */
std::uint32_t dl_fixup_count() noexcept;
} // namespace fw
