/**
 * @file symtab-blob.c
 * @brief Flash placeholder for the link-time loader symbol table.
 *
 * Reserves a fixed-size, zero-filled region in flash. After the link,
 * test/tools/gen-symtab.py renders the real table from the ELF's .symtab and
 * objcopy --update-section writes it here. The section keeps its address and
 * size across that rewrite, so nothing else moves and the addresses baked into
 * the table remain valid.
 *
 * An un-updated firmware simply has magic == 0, and dl-impl.cpp falls back to
 * reading the ELF over semihosting.
 */
#include <stdint.h>

#ifndef SCL_SYMTAB_BLOB_KB
#define SCL_SYMTAB_BLOB_KB 88
#endif

__attribute__((section(".scl_symtab"), used, aligned(4)))
const uint8_t scl_symtab_blob[SCL_SYMTAB_BLOB_KB * 1024] = { 0 };
