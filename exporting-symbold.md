
Then the recipe is short, but two of the filters can't be removed by any linker flag. Here's the maximal set, with what each one is actually undoing:

**Shared library:**
```
gcc -shared -fPIC \
    -fvisibility=default -fno-visibility-inlines-hidden \
    -Wl,--whole-archive -lyourstatic -Wl,--no-whole-archive \
    -Wl,--no-gc-sections \
    a.c b.c -o liba.so
```

**Executable:**
```
gcc -rdynamic \
    -fvisibility=default -fno-visibility-inlines-hidden \
    -Wl,--whole-archive -lyourstatic -Wl,--no-whole-archive \
    -Wl,--no-gc-sections \
    m.c -o prog
```

`-fvisibility=default` and `--no-gc-sections` are already the defaults — they're in there to defend against a build system that set the opposite. `-fno-visibility-inlines-hidden` only matters for C++ (many projects turn inline-hiding on and then wonder where their template instantiations went). `--whole-archive` is the one that usually makes the real difference, since it forces every object out of a `.a` instead of only the referenced ones.

**The two things no flag can fix:**

1. `static` functions and variables, and C++ anonymous-namespace members. These have local binding at the object-file level; the linker never sees an exportable name. Only a source change promotes them.

2. Explicit `__attribute__((visibility("hidden")))` in the source or in a header's `#pragma GCC visibility push(hidden)`. The explicit attribute beats `-fvisibility=default` — the flag only sets the *default* for unannotated symbols. Verified: a function marked hidden stays out of `.dynsym` even with `-fvisibility=default` on the command line. You'd need `-D` tricks against whatever macro the project uses (`FOO_LOCAL`, `HIDDEN`, etc.) to neutralize it.

For case 1 people sometimes reach for `-Dstatic=` on the compiler line. It does work and does export everything, but it's genuinely hazardous: function-local `static` variables become globals with fresh storage semantics, and file-scope names that collide across translation units turn into duplicate-symbol errors or, worse, silent aliasing. Fine for a one-off instrumentation build, not for anything you ship.

One check worth making before you commit to this: if the reason you want everything exported is readable stack traces from `backtrace_symbols()` or a crash handler, `-rdynamic` alone is the documented requirement and you don't need `--whole-archive` or the source surgery. If instead it's so a `dlopen`'d module can call arbitrary host internals, the above is right — just know you've now frozen every global into your ABI, so any later rename is a break for the plugin.


bare-metal cortex m

No — and the reason is structural, not a toolchain quirk. Almost none of that machinery applies to bare-metal Cortex-M.

I don't have `arm-none-eabi-gcc` in this sandbox, but the decisive behaviour is a property of static linking, which I can show with the host toolchain:

```
$ gcc -static -rdynamic m.c -o statdyn
$ readelf -S statdyn | grep -E 'dynsym|dynamic'
   (nothing — no .dynsym, no .dynamic)
$ nm -D statdyn
nm: statdyn: no symbols
```

A fully static link produces no dynamic symbol table at all, so `-rdynamic` silently accomplishes nothing. Every `arm-none-eabi` link is that case: newlib specs link static, there's no `ld.so` on the target, no `dlopen`, no interposition. `-shared` is likewise meaningless — even if you coax `ld` into emitting an ELF shared object, nothing on the chip would load it.

**What "exported" means changes.** On bare metal the table that matters is `.symtab`, and it already keeps everything — including `static` and hidden symbols:

```
$ nm statdyn | grep exe_
... T exe_global
... T exe_hidden
... t exe_static     <- local, still present
```

So for a debugger, a map file, or a symbol lookup by an external tool, you already have the maximum. Nothing needs enabling. The only thing that removes it is `-s`/`--strip-all` or `strip`.

**What carries over from the previous answer:**

- `-Wl,--whole-archive` is very much alive and is the flag you'll actually reach for. It's how you force driver-registration objects, constructor-section entries, or weak-symbol overrides out of a `.a` when nothing references them by name.
- `--gc-sections` with `-ffunction-sections -fdata-sections` is standard on Cortex-M, and here `-rdynamic` gives you no protection, since there's no `.dynsym` to root the symbols. To keep something alive you use `KEEP(...)` in the linker script, or `-Wl,--undefined=symbol` / `-u symbol` on the command line.

**What drops out entirely:** `-rdynamic`, `-shared`, `-Bdynamic`/`-Bstatic`, `-Bsymbolic`, `--version-script`, `--export-dynamic`. Visibility attributes still compile fine but have no runtime meaning — with no dynamic table, hidden vs default affects nothing after link.

If the real goal is loadable modules on Cortex-M — plugins, OTA-updatable partitions, a scripting host — that's a different design problem than symbol export. The usual routes are linking the module at a fixed address and calling through a hand-maintained function-pointer table, or writing a small in-flash ELF loader and building modules with `-Wl,--emit-relocs` (`-Wl,-q`) so the relocations survive for you to apply at load time. Worth saying which of those you're aiming at, since the flags diverge sharply from there.
