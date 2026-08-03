#!/usr/bin/env python3
# embed-blob.py <input.rel.o> <symbol> <output.c>
# Emit a C file with the module bytes as an 8-aligned const array in flash
# (.rodata): const unsigned char <symbol>_rel_o[]; const unsigned <symbol>_rel_o_len;
import sys
src, sym, out = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(src, "rb").read()
with open(out, "w") as f:
    f.write("/* generated from %s by embed-blob.py — do not edit */\n" % src)
    f.write('__attribute__((aligned(8))) const unsigned char %s_rel_o[] = {\n' % sym)
    for i in range(0, len(data), 16):
        f.write("  " + ",".join("0x%02x" % b for b in data[i:i+16]) + ",\n")
    f.write("};\n")
    f.write("const unsigned %s_rel_o_len = %d;\n" % (sym, len(data)))
print("embedded %s (%d bytes) -> %s" % (sym, len(data), out))
