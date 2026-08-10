# AetherELF GUI Visualizer — Usage & Integration Guide

The **AetherELF** web interface is a futuristic, glassmorphic workspace designed to analyze, visualize, and trace the structures of 32-bit ARM ELF binaries. It allows you to inspect headers, map section overlays, examine procedure link tables, hex-dump binary offsets, and trace execution flow.

This GUI is integrated with the **common-elf-reader** library (git submodule) for reliable, server-side ELF parsing via the `elf2json` tool.

---

## 1. Getting Started

The visualizer is located at `gui/index.html`. Since the GUI runs entirely on client-side JavaScript, you do not need to set up any backend databases.

### Launching the Interface
*   **Method A (Direct Open)**: Open `gui/index.html` in your browser.
*   **Method B (Local Server)**: Start a local HTTP server in the repository root to access it via localhost:
    ```bash
    cd shared-loaders-cortex-m-qemu
    python3 -m http.server 8000
    # Then navigate to http://localhost:8000/gui/
    ```

---

## 2. Building the elf2json Tool

The `elf2json` tool uses **common-elf-reader** to parse ARM ELF32 binaries and export them as JSON for the GUI.

### Option A: Using the build script
```bash
cd tools
./build.sh
# Produces: tools/elf2json and tools/gen-symtab
```

### Option B: Using CMake
```bash
cmake -B build
cmake --build build --target elf2json-host
# Produces: build/tools/elf2json
```

---

## 3. Loading ELF Data

The GUI supports three flexible data loading strategies:

### Option A: Upload Raw ELF Binaries (Recommended)
Directly drag-and-drop any ARM32 ELF target binary into the **Upload ARM32 ELF** dashed zone on the sidebar.
*   *How it works*: An integrated, pure-JavaScript `Elf32Parser` parses the binary structure (headers, program load segments, symbols, relocations, dynamic entries, and raw section bytes) on the fly directly inside the browser.

### Option B: Use elf2json to Export from common-elf-reader (Recommended for complex binaries)
Export an ELF file to JSON using the `elf2json` tool, then load it into the GUI:
```bash
# Build the tool
cd tools && ./build.sh

# Export ELF to JSON
./elf2json /path/to/firmware.elf firmware.json
```
Then either:
*   Drag-and-drop `firmware.json` into the GUI's upload zone, OR
*   Open `firmware.json` in a text editor, copy the contents, paste them into the **Paste JSON Export** textbox on the sidebar, and click **Load Paste JSON**.

### Option C: Paste C++ Parser JSON Export
You can generate a database export of any binary using the modernized C++ parser in JSON mode:
```bash
./elf2json test_arm.elf gui/test_arm.json
```
Open `gui/test_arm.json` in a text editor, copy the contents, paste them into the **Paste JSON Export** textbox on the sidebar, and click **Load Paste JSON**.

---

## 4. JSON Format

The `elf2json` tool produces JSON with the following structure:
```json
{
  "header": { "type", "machine", "entry", "phoff", "shoff", "phnum", "shnum" },
  "program_headers": [ { "type", "offset", "vaddr", "paddr", "filesz", "memsz", "flags", "align" } ],
  "section_headers": [ { "index", "name", "type", "flags", "addr", "offset", "size", "link", "info", "entsize" } ],
  "dynamic": [ { "tag", "val" } ],
  "plt": [ { "offset", "info", "section" } ],
  "symbols": [ { "name", "value", "size", "bind", "type", "shndx", "source" } ]
}
```

---

## 5. Interface Layout & Navigation Tabs

Once a binary is loaded, the visualizer unlocks 6 analysis tabs:

### 1. Overview Tab
*   **Virtual Address Memory Map**: Renders an interactive bar representation of the load segments. Nested sections are drawn proportional to their sizes inside their respective segment block.
*   **Interactive Jumping**: Click any section block (e.g. `.text` or `.got`) or symbol listed in the summary cards to navigate to its hex dump/tracer view.

### 2. Segments Tab
*   Displays the **Program Headers** table, defining the execution view (segments like `PT_LOAD`, `PT_DYNAMIC`, `PT_INTERP`).
*   The **Virtual Address** (`V-Addr`) is linkified. Clicking it jumps directly to the tracer to inspect the segment's start boundary.

### 3. Sections Tab
*   Lists the **Section Headers** table with sorting and filtering options (e.g., filtering for `PROGBITS`, `SYMTAB`, `NOBITS`).
*   Clicking a **Section Name** or **V-Addr** link routes you to the tracer to dump the section.

### 4. Symbols Tab
*   Searchable and filterable index of all symbol table entries (`.symtab` and `.dynsym`).
*   Allows filtering symbols by type (`FUNC`, `OBJECT`) or binding (`GLOBAL`, `LOCAL`, `WEAK`).
*   Clicking **Trace** on any symbol jumps to its virtual address in the tracer.

### 5. Address Tracer & Hex Tab
*   This is the core analysis panel. It contains the **Address Tracer**, **Hex Viewer**, and **Instruction Disassembler** working in unison.
*   See *Section 6* below for detailed utilization of the tracer.

### 6. Relocs & Dynamics Tab
*   **Dynamic Entries**: Decodes the `.dynamic` section. Memory-pointed dynamic tags (such as `DT_PLTGOT`, `DT_SYMTAB`, etc.) are clickable links.
*   **Relocations**: Lists `.rel.dyn` and `.rel.plt` patches. The patch **Offset** and target **Mapped Symbol** names are clickable links.
*   **PLT Map**: Details PLT entry slots, resolving branch destinations to clickable symbol targets.

---

## 6. Tracing Pointers & "Stepping Into" Binaries

The visualizer implements a browser-like navigation paradigm for reverse-engineering:

```
  [Segment/Section Link] ──► [Tracer Address Resolve] ──► [Highlight Bytes]
                                     │                           │
                                     ▼                           ▼
                           [Disasm Target Link]         [Hex Pointer Word]
```

### Addressing History Stack (◀ / ▶)
A browser-style navigation history tracks your inspection sequence. If you click on pointers inside the Hex Viewer or target links in the disassembler, you can easily step backward (`◀`) and forward (`▶`) using the history buttons next to the address input bar without getting lost in the binary.

### Inline Pointer Jumping in Hex Viewer
*   The Hex Viewer groups bytes into **4-byte words** (aligned to word boundaries).
*   If a 4-byte word represents a pointer that matches a symbol address or falls within a section boundary, it is styled with a **dotted green underline** and displays a tooltip describing the destination (e.g. `Symbol: main (0x8120)`).
*   **Clicking the word** performs a "step into" operation, jumping the Hex Viewer directly to that address.

### Clickable Disassembly Targets
*   The **Instruction Disassembler** translates machine opcodes (ARM or Thumb) on-board.
*   It resolves branch targets (e.g. `b`, `bl`, `bne`) and literal constant load targets (e.g. `ldr r0, [pc, #offset]`), linking them to their corresponding symbols.
*   The target symbol or address comment (e.g. `; -> main`) is rendered as a **clickable link**. Click it to jump directly to the target function's disassembly and hex dump.
