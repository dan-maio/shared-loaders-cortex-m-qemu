# Integrating `common-elf-reader`

This branch (`common_elf_reader`) begins migrating this project's ELF
reader/parser onto the shared standalone component
[`common-elf-reader`](../../common-elf-reader) so that both this repo and
`cortexm-shared-loader` parse ELF objects through **one API**
(`loader::elf_image` + `loader::byte_source` + `loader::result<T>`).

Only the **reader/parser** moves. The relocation/patching and symbol-resolving
code stays here and is modified only as far as needed to feed off the new
parser:

- keep: `elf_loader::SymbolTable`, `ShlibLoader::patch_shlib`/`patch_main`,
  `read_relocation_symbols`, `read_dynamic_symbols`,
  `calculate_patch_array_size`, `find_symbol_address`
- replace: the file-I/O + ELF-structure parsing currently inside
  `ElfReader` (magic/class/machine/type validation, header/phdr/shdr reads,
  section lookup, `image_size`, segment loading, `.dynamic` scan)

## Wiring plan (incremental)

1. **Vendor the dependency.** Add `common-elf-reader` as a sibling path (or git
   submodule) and build it **by source, not as a static archive**:
   - **Makefile:** add `-I../../common-elf-reader/include` and list
     `../../common-elf-reader/src/elf-image.cpp` among the sources.
   - **CMake:** `add_subdirectory(common-elf-reader)` and link the
     `common-elf-reader::elf-reader` **INTERFACE** target
     (`target_link_libraries(f411ce-sh PRIVATE common-elf-reader::elf-reader)`).
     The target carries `elf-image.cpp` and the `include/` dir as INTERFACE
     properties, so the parser is compiled into the firmware with its own flags
     — no `.a` is produced.

2. **Back `ElfReader` with `elf_image`.** Give `ElfReader` a
   `loader::fd_byte_source` + `std::optional<loader::elf_image>` instead of a
   raw `fd`. Re-express the parsing methods as thin adapters:
   - `open_elf` → `fd_byte_source::open_file` + `elf_image::parse`
   - `verify_elf` → map `elf_image::parse` errors to `LoaderError`
   - `get_header` / `read_program_header` / `read_section_header` →
     `elf_image::header/program_header/section_header` (layout-identical to the
     `<elf.h>` `Elf32_*` structs; copy across)
   - `find_section_by_name` / `read_helper_sections` → `elf_image::find_section`
   - `get_binary_size` → `elf_image::image_size`
   - `load_segments`, `read_section_content`, `scan_dependencies` →
     `img.source().read_at(...)`

3. **Keep the resolving/patching API unchanged.** Those methods operate on
   already-loaded RAM (`PassArgs`), not the file, so they need no change beyond
   the byte reads they already do.

4. **Delete** the now-unused raw `fd`/`lseek`/`read` plumbing from
   `elf-reader.cpp` once step 2 lands.

5. **Verify** with `make clean && make` and a semihosting run; the loader output
   (GOT dump, constructor prints, library calls) should be unchanged.

## Status

- [x] Standalone `common-elf-reader` repo created, builds (host + cortex-m4),
      smoke-tested against `libsh1.so`. Pushed to
      `/home/dan/Downloads/GIT/elf32_read_parse.git`.
- [x] Step 1 — vendored via `-I../../common-elf-reader/include` and compiling
      `../../common-elf-reader/src/elf-image.cpp` (see `Makefile`).
- [x] Step 2 — `ElfReader` is now a thin adapter over `loader::elf_image` +
      `loader::fd_byte_source` (`elf-reader.h`/`elf-reader.cpp`). Resolving and
      patching are unchanged. `loader/*` headers are included **before**
      `<elf.h>` to avoid the macro/constexpr name clash (SHF_ALLOC, PT_LOAD, …).
- [x] Step 4 — raw `fd`/`open`/`read`/`lseek` plumbing removed from
      `elf-reader.cpp`; the fd now lives in `loader::fd_byte_source`.
- [x] Step 5 — on-target semihosting verification: ran on the WeAct BlackPill STM32F411CE via
      `build-and-run.sh`; output identical to the pre-migration in-house parser
      (segment dumps, constructor prints, GOT walkthrough, library calls).

## Notes for the second consumer (`cortexm-shared-loader`)

Done: `cortexm-shared-loader` now consumes the same shared copy. Its
`scl-common` links `common-elf-reader::elf-reader` (via `add_subdirectory`), so
`elf-image.cpp` and the `include/loader/` headers propagate from the one repo —
compiled from source (INTERFACE, no archive), exactly as here. Its local
`elf-image.*`, `byte-source.hpp`, `elf-types.hpp`, `error.hpp` (and the shared
`detail/bytes.hpp`) were dropped in favour of the shared repo.
