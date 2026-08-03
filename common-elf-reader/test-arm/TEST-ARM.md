# common-elf-reader test-arm — Documentation

## Overview

The `test-arm` directory contains a **bare-metal ARM test** for the
`common-elf-reader` ELF32 parser library. It runs on a **WeAct STM32F411CE
BlackPill** board (Cortex-M4F @ 100 MHz) and exercises the full parser on a
real ELF binary loaded via ARM semihosting.

This is the cross-compilation counterpart to the host-side `test/` directory:
same test logic, real hardware, real semihosting I/O, no OS.

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│  Host (OpenOCD)                                              │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │  openocd -f openocd.cfg                                 │ │
│  │    -c "arm semihosting_cmdline ... 411ce-100Mhz.elf"    │ │
│  │    -c "flash write_image erase elf-reader-test-arm"     │ │
│  │    -c "reset run"                                       │ │
│  └──────────────────────────┬──────────────────────────────┘ │
│                              │ SWD (CMSIS-DAP)               │
└──────────────────────────────┼───────────────────────────────┘
                               │ BKPT 0xAB (semihosting)
┌──────────────────────────────┴───────────────────────────────┐
│  Target: WeAct STM32F411CE                                   │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │  startup.c        Reset_Handler → copy .data → .bss →   │ │
│  │                   FPU enable → main()                    │ │
│  │  board.c          HSE 25 MHz → PLL → 100 MHz            │ │
│  │                   SysTick 1 ms, GPIO, USART1, SPI1       │ │
│  │  semihost.c       BKPT 0xAB: open/read/seek/flen/close  │ │
│  │  libc-shim.c      memcpy, memset, strlen, strcmp, abort  │ │
│  │  printf-shim.c    %s %c %u %d %x %p %% over semihosting │ │
│  │  test-main.cpp    semihost_byte_source + 8 test groups   │ │
│  └─────────────────────────────────────────────────────────┘ │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │  common-elf-reader (INTERFACE source library)            │ │
│  │    elf_image::parse()                                    │ │
│  │    program_header() / section_header() / find_section()  │ │
│  │    image_size() / image_base()                           │ │
│  │    detail::load_word / store_word / name_at              │ │
│  └─────────────────────────────────────────────────────────┘ │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │  Fixture: 411ce-100Mhz.elf (25 KB, stripped)            │ │
│  │  Loaded via semihost_open("411ce-100Mhz.elf")           │ │
│  └─────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────┘
```

## File Structure

```
test-arm/
├── cmake/
│   ├── arm-none-eabi.cmake         # Toolchain: xPacks GCC 15.2.1, Cortex-M4 HF
│   └── platform-library.cmake      # Sources all platform files from test-arm/src/
├── CMakeLists.txt                  # Builds elf-reader-test-arm, copies fixture
├── include/
│   ├── board.h                     # board_init, LED, UART, SPI, SysTick, millis
│   └── semihost.h                  # trace_puts, semihost_open/read/seek/flen/close
├── ldscripts/
│   ├── mem.ld                      # FLASH 512K @ 0x08000000, RAM 128K @ 0x20000000
│   └── sections.ld                 # .isr_vector → FLASH, .data/.bss → RAM
├── src/
│   ├── startup.c                   # Vector table + Reset_Handler
│   ├── board.c                     # Clock config (100 MHz), GPIO, SysTick
│   ├── semihost.c                  # BKPT 0xAB semihosting I/O
│   ├── libc-shim.c                 # Freestanding memcpy/memset/strlen/strcmp
│   ├── printf-shim.c               # Minimal printf over semihosting
│   └── test-main.cpp               # 63 assertions in 8 test groups
├── 411ce-100Mhz-stripped.elf       # 25 KB stripped fixture (no debug info)
├── 411ce-100Mhz.hex                # Intel HEX for flashing
├── openocd.cfg                     # CMSIS-DAP @ 1 MHz SWD
├── build.sh                        # cmake configure + build
└── run-test.sh                     # flash + semihosting run + pass/fail check
```

## Platform Components

### startup.c — Vector Table and Reset Handler

The Cortex-M4 vector table occupies the first 512 bytes of FLASH
(`.isr_vector`). Entry 0 is the initial stack pointer (top of RAM),
entry 1 is `Reset_Handler`.

`Reset_Handler` (naked function):
1. Copies `.data` from its FLASH load image to RAM (`_sidata` → `_sdata.._edata`)
2. Zeroes `.bss` (`_sbss.._ebss`)
3. Enables the FPU (CPACR bits 20-23)
4. Calls `main()`
5. Spins forever if `main()` returns

All unused IRQ handlers are weak aliases to `Default_Handler` (trace + spin).

### board.c — Clock Configuration

The WeAct BlackPill has a **25 MHz HSE crystal**. The PLL configuration:

```
HSE (25 MHz) → /PLLM(25) = 1 MHz → ×PLLN(200) = 200 MHz → /PLLP(2) = 100 MHz
```

| Clock | Frequency |
|-------|-----------|
| SYSCLK | 100 MHz |
| AHB (HCLK) | 100 MHz (/1) |
| APB1 | 50 MHz (/2) |
| APB2 | 100 MHz (/1) |
| Flash | 3 wait states |

`SysTick` is configured for 1 ms interrupts. `board_init()` also enables
GPIOA/C clocks and configures PC13 as LED output.

### semihost.c — Semihosting I/O

ARM semihosting lets the target request host services via `BKPT 0xAB`.
OpenOCD intercepts these calls and performs the I/O on the host.

| Function | Semihosting Op | Description |
|----------|---------------|-------------|
| `trace_puts(s)` | SYS_WRITE0 | Write NUL-terminated string to debug console |
| `trace_putc(c)` | SYS_WRITEC | Write one character |
| `semihost_open(path, mode)` | SYS_OPEN | Open file (mode 1 = "rb") |
| `semihost_flen(fd)` | SYS_FLEN | Get file length |
| `semihost_seek(fd, pos)` | SYS_SEEK | Seek to absolute position |
| `semihost_read(fd, buf, len)` | SYS_READ | Read bytes (returns bytes NOT read) |
| `semihost_close(fd)` | SYS_CLOSE | Close file handle |
| `semihost_get_cmdline(buf, len)` | SYS_GET_CMDLINE | Read host command line |

### libc-shim.c — Freestanding C Library

GCC lowers struct copies and `std::memcpy`/`std::memset` into libc calls.
With `-nostdlib`, these would be undefined. This shim provides:

`memcpy`, `memset`, `memmove`, `memcmp`, `strlen`, `strcmp`, `strncmp`, `abort`

Built with `-fno-tree-loop-distribute-patterns` to prevent GCC from
turning these loops back into memcpy/memset calls.

### printf-shim.c — Minimal printf

A ~50-line `printf`/`puts` implementation supporting `%s %c %u %d %x %p %%`.
Output goes through `trace_putc`/`trace_puts` (semihosting).

## The Test — test-main.cpp

### semihost_byte_source Adapter

The `common-elf-reader` library reads ELF files through the abstract
`loader::byte_source` interface (`read_at(offset, span)`).

`semihost_byte_source` implements this interface using the semihosting
file I/O functions:

```
open_file(path)  → semihost_open + semihost_flen
read_at(off, sz) → semihost_seek(off) + semihost_read(buf, sz)
~destructor()    → semihost_close
```

This lets the parser read the 25 KB ELF fixture directly from the host
filesystem — no RAM buffer needed, no filesystem on the target.

### Test Groups

Each test group prints `[TEST] <name>`, runs assertions, and prints
`-> N ok` (and `M FAIL` if any failed).

#### 1. byte_source: open file, read, bounds (5 checks)

| Check | What |
|-------|------|
| `size>0` | File has nonzero length |
| `read_at(0,4)` | Can read first 4 bytes |
| `magic=0x7fELF` | Bytes are `\x7fELF` |
| `read-past-EOF-fails` | Read beyond size returns error |
| `err=SHORT_READ` | Error code is SHORT_READ |

#### 2. elf_image::parse: validate Ehdr fields (10 checks)

| Check | What |
|-------|------|
| `magic` | e_ident[0..3] = 0x7f E L F |
| `ELFCLASS32` | e_ident[4] = 1 (32-bit) |
| `ELFDATA2LSB` | e_ident[5] = 1 (little-endian) |
| `EM_ARM=40` | e_machine = 40 (ARM) |
| `ET_EXEC` | e_type = 2 (executable) |
| `Ehdr=52bytes` | e_ehsize = 52 |
| `4 program headers` | e_phnum = 4 |
| `section headers > 0` | e_shnum > 0 |
| `shstrndx valid` | e_shstrndx < e_shnum |
| `entry=0x08000199` | Entry point matches |

#### 3. program_header: iterate PT_LOAD segments (9 checks)

Iterates all 4 program headers. Verifies:
- Each reads successfully (`phdr-read`)
- PT_LOAD segments have `memsz >= filesz`
- Exactly 3 PT_LOAD segments exist (FLASH code, FLASH→RAM data, RAM BSS)
- Out-of-range index returns error

#### 4. section_header: iterate all, bounds check (15 checks)

Reads all 13 section headers. Verifies:
- Each reads successfully (`shdr-read`)
- At least one SHT_NULL section exists
- Out-of-range index returns error

#### 5. find_section: lookup by name (10 checks)

Looks up 6 known sections by name: `.text`, `.data`, `.bss`,
`.isr_vector`, `.ARM.exidx`, `.shstrtab`.

Also verifies:
- Missing section returns `BAD_SECTION` error
- `.text` address is `0x08000440`
- `.text` size > 0

#### 6. image_size / image_base from PT_LOAD (4 checks)

| Check | Value |
|-------|-------|
| `image_size ok` | Call succeeds |
| `size=0x20000528` | Highest PT_LOAD end (RAM BSS top) |
| `image_base ok` | Call succeeds |
| `base=0x08000000` | Lowest PT_LOAD vaddr (FLASH start) |

#### 7. source().read_at: read raw section bytes (3 checks)

Reads the `.comment` section content via `img.source().read_at()`.
Verifies the section exists, read succeeds, and content is non-zero.

#### 8. detail::bytes: load/store/name_at helpers (7 checks)

Tests the bounds-checked byte-access helpers:
- `store_word` + `load_word` round-trip at offset 0
- `load_word` past span returns 0
- `store_word` past span is a no-op
- `load_byte` at valid offset
- `load_byte` past span returns 0
- `name_at` finds "bar" at offset 4 in "foo\0bar\0baz"
- `name_at` past span returns empty view

## Build & Run

### Prerequisites

- **arm-none-eabi-g++** 15.2.1 (xPacks)
- **OpenOCD** 0.12.0 (xPacks)
- **WeAct STM32F411CE** connected via USB (CMSIS-DAP)
- **CMake** 3.16+

### Build

```sh
cd common-elf-reader/test-arm
./build.sh                  # default: ./build
./build.sh /tmp/my-build    # custom build dir
```

### Flash & Run

```sh
./run-test.sh               # default: ./build
./run-test.sh /tmp/my-build # custom build dir
```

### Manual OpenOCD

```sh
cd build
openocd -f ../openocd.cfg \
    -c "reset halt" \
    -c "arm semihosting enable" \
    -c "arm semihosting_cmdline elf-reader-test-arm 411ce-100Mhz.elf" \
    -c "flash write_image erase elf-reader-test-arm" \
    -c "reset run"
```

### Expected Output

```
=== common-elf-reader test-arm (WeAct STM32F411CE) ===
fixture: 411ce-100Mhz.elf
[TEST] 1. byte_source: open file, read, bounds
  -> 5 ok
[TEST] 2. elf_image::parse: validate Ehdr fields
  -> 10 ok
[TEST] 3. program_header: iterate PT_LOAD segments
  -> 9 ok
[TEST] 4. section_header: iterate all, bounds check
  -> 15 ok
[TEST] 5. find_section: lookup by name
  -> 10 ok
[TEST] 6. image_size / image_base from PT_LOAD
  -> 4 ok
[TEST] 7. source().read_at: read raw section bytes
  -> 3 ok
[TEST] 8. detail::bytes: load/store/name_at helpers
  -> 7 ok

=== 63 passed, 0 failed ===
```

## Linker Scripts

### mem.ld — Memory Map

```
FLASH (rx)  : 0x08000000, 512K
RAM   (rwx) : 0x20000000, 128K
_estack     = 0x20020000    (top of RAM, initial SP)
```

### sections.ld — Section Placement

| Section | Load Address | Run Address | Description |
|---------|-------------|-------------|-------------|
| `.isr_vector` | FLASH | FLASH | Interrupt vector table (16 entries) |
| `.text` | FLASH | FLASH | Code + rodata |
| `.ARM.exidx` | FLASH | FLASH | Exception unwinding tables |
| `.data` | FLASH (LMA) | RAM | Initialized data (copied at boot) |
| `.bss` | — | RAM | Zero-initialized data |
| `.image_arena` | — | RAM | 64 KB arena for loaded library images |

## Fixture: 411ce-100Mhz-stripped.elf

The test fixture is a **stripped** version of the firmware ELF (25 KB,
no debug sections). It is a real STM32F411CE executable with:

- 4 program headers (ARM_EXIDX, 3× PT_LOAD)
- 13 section headers (after stripping)
- `.text` at 0x08000440, `.data` at 0x20000000, `.bss` at 0x20000070
- Entry point at 0x08000199

The stripped ELF is copied to the build directory as `411ce-100Mhz.elf`
by CMake's `configure_file()`. OpenOCD's semihosting resolves the
filename relative to its working directory (the build dir).

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| `unable to find a matching CMSIS-DAP device` | Board not connected | Check USB cable (must be data, not charge-only) |
| `Error: BUG: can't assert SRST` | No reset line | Remove `reset_config connect_assert_srst` from openocd.cfg |
| `Error: Target not halted` | Target running before flash | Add `reset halt` before `flash write_image` |
| `cannot open 411ce-100Mhz.elf` | Fixture not in build dir | Run `./build.sh` first (copies fixture via configure_file) |
| `timed out while waiting for target halted` | Test spins after exit | Expected — startup.c loops after main() returns |
| `FAIL shnum24` | Using unstripped .elf | Use `411ce-100Mhz-stripped.elf` (13 sections, not 24) |
