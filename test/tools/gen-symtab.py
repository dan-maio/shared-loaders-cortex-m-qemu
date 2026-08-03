#!/usr/bin/env python3
"""gen-symtab.py — build the firmware's dynamic-loader symbol table at link time.

The loader resolves a library's imports against the firmware's own symbols. Doing
that at runtime means reading .symtab + .strtab out of the firmware ELF over
semihosting on every boot (~160 KB, minutes on a slow probe) and keeping the
77 KB .strtab alive in RAM for the lifetime of the program, because the symbol
entries point into it. On a 128 KB part that does not fit alongside a dlopen.

Instead we emit the table into flash at link time. The firmware reserves a
fixed-size, zero-filled placeholder section (.scl_symtab); this script reads the
linked ELF, renders the real table, and objcopy --update-section overwrites the
placeholder in place. Because the section keeps its address and size, no other
symbol moves, so the addresses baked into the table stay correct.

Blob layout (little-endian), matching firmware::sym_entry:

    +0   u32  magic 'SCLS'
    +4   u32  count
    +8   { u32 name;  u32 addr; } * count   -- `name` is an ABSOLUTE flash
                                               address, so the array can be
                                               handed to set_symtab() as-is
    ...  NUL-terminated name strings
    ...  zero padding out to the placeholder size

Usage: gen-symtab.py <firmware.elf> <out.bin>
"""
import struct
import sys

MAGIC = 0x534C4353  # 'SCLS' little-endian
SECTION = ".scl_symtab"

STB_GLOBAL, STB_WEAK = 1, 2
SHN_UNDEF, SHN_LORESERVE = 0, 0xFF00


def parse(path):
    d = open(path, "rb").read()
    if d[:4] != b"\x7fELF":
        sys.exit(f"gen-symtab: {path} is not an ELF file")
    e_shoff, = struct.unpack_from("<I", d, 0x20)
    e_shentsize, = struct.unpack_from("<H", d, 0x2E)
    e_shnum, = struct.unpack_from("<H", d, 0x30)
    e_shstrndx, = struct.unpack_from("<H", d, 0x32)

    def shdr(i):
        f = struct.unpack_from("<10I", d, e_shoff + i * e_shentsize)
        # name, type, flags, addr, offset, size, link, info, align, entsize
        return dict(zip(
            "name type flags addr offset size link info align entsize".split(), f))

    shstr = shdr(e_shstrndx)["offset"]
    secs = {}
    for i in range(e_shnum):
        s = shdr(i)
        end = d.index(b"\0", shstr + s["name"])
        secs[d[shstr + s["name"]:end].decode()] = s
    return d, secs


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    elf_path, out_path = sys.argv[1], sys.argv[2]
    d, secs = parse(elf_path)

    for need in (SECTION, ".symtab", ".strtab"):
        if need not in secs:
            sys.exit(f"gen-symtab: {elf_path} has no {need} section")

    slot = secs[SECTION]
    symtab, strtab = secs[".symtab"], secs[".strtab"]

    # Locals occupy [0, sh_info) and are never importable; skip them, matching
    # the runtime filter this replaces.
    first = symtab["info"]
    count = symtab["size"] // 16
    if first >= count:
        first = 0

    syms = []
    seen = set()
    for i in range(first, count):
        o = symtab["offset"] + i * 16
        st_name, st_value, _sz, st_info, _other, st_shndx = \
            struct.unpack_from("<IIIBBH", d, o)
        if st_value == 0:
            continue
        if st_shndx == SHN_UNDEF or st_shndx >= SHN_LORESERVE:
            continue
        if (st_info >> 4) not in (STB_GLOBAL, STB_WEAK):
            continue
        if st_name == 0 or st_name >= strtab["size"]:
            continue
        end = d.index(b"\0", strtab["offset"] + st_name)
        name = d[strtab["offset"] + st_name:end]
        if not name:
            continue
        # Filter out compiler/runtime internal symbols to keep the table size compact
        if name.startswith(b"_") and not any(name.startswith(w) for w in [
            b"_impure_ptr", b"_Znw", b"_Zdl", b"__cxa", b"__errno",
            b"__exidx", b"__gnu_Unwind", b"__register_frame_info", b"__deregister_frame_info"
        ]):
            continue
        # A later duplicate would only ever shadow the first; keep one.
        if name in seen:
            continue
        seen.add(name)
        syms.append((name, st_value))

    header = 8
    table = len(syms) * 8
    names_off = header + table

    blob = bytearray()
    names = bytearray()
    blob += struct.pack("<II", MAGIC, len(syms))
    for name, addr in syms:
        blob += struct.pack("<II", slot["addr"] + names_off + len(names), addr)
        names += name + b"\0"
    blob += names

    if len(blob) > slot["size"]:
        sys.exit(
            f"gen-symtab: table is {len(blob)} bytes but the {SECTION} "
            f"placeholder is only {slot['size']}.\n"
            f"  Raise SCL_SYMTAB_BLOB_KB to at least "
            f"{(len(blob) + 1023) // 1024} and rebuild.")

    blob += b"\0" * (slot["size"] - len(blob))
    open(out_path, "wb").write(blob)
    print(f"gen-symtab: {len(syms)} symbols, {names_off + len(names)} bytes "
          f"used of {slot['size']} at {slot['addr']:#010x}")


if __name__ == "__main__":
    main()
