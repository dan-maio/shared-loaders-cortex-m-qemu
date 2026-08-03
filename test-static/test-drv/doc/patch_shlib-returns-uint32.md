# `patch_shlib()` Returns `uint32_t`, Not `bool` — A Subtle Bug

## The Bug

`shlib_loader::patch_shlib()` returns `uint32_t` — the number of symbols successfully
patched. A return value of **0 means "nothing to patch"**, which is perfectly normal
(e.g. when `populate_helper_sections()` hasn't been called, or when the `.so` has zero
relocations).

However, the `test-drv` code treated it as a boolean:

```cpp
if (!shl.patch_shlib(&g_syms)) return 0;   // BUG
```

Since `0` patched symbols → `!0` = `true` → the function returned early with failure,
even though everything before it (open, verify, load_bin) had succeeded.

## Why 0 Is Normal

The `scl::static` loader only patches symbols when both sides (main firmware and loaded
`.so`) have relocation / symbol-table records. The loader is designed for a **build-time
linked** workflow:

1. Main firmware is linked with `-lsh1 -lsh2` → linker emits `.dynsym`/`.dynstr`/`.rel.dyn`
   and a `_reloc` block in the main binary.
2. At runtime, `populate_helper_sections()` reads `_reloc` to find main's dynamic sections,
   then `get_shlib_patch_count()/get_main_patch_count()` count relocation entries.
3. `patch_shlib()` and `patch_main()` patch those pre-existing GOT slots.

For the `test-drv` project, the firmware is **not** linked with `-lspi -luart -lled`.
The `.so` files are loaded purely at runtime without build-time linkage. The loader sees
zero relocation entries (both `m_shlib` and `m_main` are empty because
`populate_helper_sections()` was never called), so it patches zero symbols — and that is
correct.

## The Fix

Remove the boolean check and just call `patch_shlib()` for its side effects (which in
this case are none, but the call must be made for API compatibility):

```cpp
shl.patch_shlib(&g_syms);           // OK — 0 patched is not an error
```

The same applies to `patch_main()` (also returns `uint32_t`).

## How to Distinguish "Zero Patched" from "Error"

`patch_shlib()` and `patch_main()` never return an error code — they log warnings for
individual bad entries and continue. The return value is purely informational. To detect
actual loading failures, check the return values of:

| Call | Returns | Failure indicator |
|------|---------|-------------------|
| `open_elf()` | `bool` | `false` |
| `load_bin()` | `loader_error` | not `OK` |
| `populate_helper_sections()` | `loader_error` | not `OK` |
| `patch_shlib()` | `uint32_t` | never fails (0 = nothing to patch) |
| `patch_main()` | `uint32_t` | never fails (0 = nothing to patch) |

## Lesson

Always check the API signature. `patch_shlib` was changed from `void` → `uint32_t` in a
refactoring pass (documented in the manual §13.3). The old `void` return couldn't be
misused as a boolean; the new `uint32_t` invites exactly this mistake when a caller
assumes a "zero == failure" convention.
