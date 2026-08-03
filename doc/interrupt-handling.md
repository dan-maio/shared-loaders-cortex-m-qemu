# Advanced Interrupt Handling and Context Switching in Dynamically Loaded Modules (Cortex-M)

When developing bare-metal or real-time embedded systems that support dynamic code loading (such as ELF shared libraries or relocatable objects), handling hardware interrupts (IRQs) within loaded modules introduces significant architectural challenges.

This document details the fundamental registers of Cortex-M, the design patterns to handle interrupts, preemptive Hard-FPU context switching, and handling timer interrupts during semihosting-based debugging.

---

## 1. The Fundamental Architecture Obstacle: The `r10` GOT Base

In Cortex-M microcontrollers, dynamically loaded shared libraries (`.so` / `ET_DYN`) are compiled with Position-Independent Code flags:
* `-fPIC` (or `-fpic`)
* `-msingle-pic-base` (which enforces a single Global Offset Table per module)

In this compile model, access to all global variables, static variables, and external functions is resolved relative to a designated **base register** (specifically `r10` / `sl` in the GNU ARM toolchain). 

### The Interrupt Entry Conflict
When a peripheral triggers an interrupt, the Cortex-M core performs the following hardware sequence:
1. **Context Stacking**: The CPU automatically pushes caller-saved registers (`r0`-`r3`, `r12`, `lr`, `pc`, `xPSR`) to the active stack pointer (`MSP` or `PSP`).
2. **Execution Switch**: The core switches to **Handler Mode** using the Main Stack Pointer (`MSP`).
3. **ISR Vector Branch**: The CPU loads the target address from the vector table (pointed to by the `VTOR` register) and branches directly to it.

**Crucially, the hardware does not modify or load `r10`.** The `r10` register retains whatever value was present at the exact clock cycle the interrupt occurred. This could be:
* The main firmware's GOT base address.
* The GOT base of a completely different dynamic module.
* Uninitialized garbage.

If the hardware branches directly to an ISR inside the loaded library, the C/C++ function will attempt to read or write its global variables using `r10`. Because `r10` points to the wrong GOT (or invalid memory), this instantly triggers a **UsageFault**, **BusFault**, or **HardFault**.

---

## 2. Design Patterns for Interrupt Handling

Depending on your loader flavor (freestanding dynamic, hosted dynamic, or relocatable), choose one of the following implementation patterns.

### Pattern A: Main Firmware Trampoline (Recommended for `scl::dynamic-*`)

The hardware vector table resides permanently in the main firmware. The main firmware exposes a static ISR that acts as an execution trampoline, manually configuring the dynamic library's register context before executing the handler.

```c
/* Main Firmware Source */

// Pointers resolved during dynamic module loading
extern void* g_fixture_got_base;         // The library's got_base (r10 target)
extern void (*g_fixture_isr_handler)(void); // The target library function

/**
 * @brief Hardware ISR wrapper defined in the main vector table
 */
void USART1_IRQHandler(void) {
    if (g_fixture_isr_handler && g_fixture_got_base) {
        // Save current r10 in r4 (callee-saved), set new r10, branch, restore r10
        __asm__ volatile (
            "mov r4, r10\n"           // Save current r10 to r4
            "mov r10, %0\n"           // Load fixture GOT base into r10
            "blx %1\n"                // Call the fixture ISR handler
            "mov r10, r4\n"           // Restore original r10
            :
            : "r"(g_fixture_got_base), "r"(g_fixture_isr_handler)
            : "r4", "lr", "memory"
        );
    }
}
```

### Pattern B: Direct Vector Table Patching with Relocatable Objects (`scl::reloc`)

If your architecture requires dynamically registering interrupts directly with the hardware (e.g., zero-latency interrupt execution without trampoline overhead), you should compile modules as **Relocatable Objects** (`.rel.o` / `ET_REL`) rather than shared libraries.

#### How `scl::reloc` Solves the Register Dependency
* **No PIC Requirements**: Relocatable modules are compiled with `-fno-pic -mlong-calls -r`. 
* **Absolute Linking at Load Time**: The relocatable loader (`scl::reloc`) allocates raw memory segments for the module's sections (`.text`, `.data`, `.bss`) and applies relocations directly in place. All relative offsets are converted into absolute address instructions.
* **No `r10` Dependency**: Since no GOT is used, functions inside relocatable modules do not rely on a base register. They are normal, absolute function pointers.

```c
/* Main Firmware Source Vector Table Patching */
void register_dynamic_irq(IRQn_Type irq_type, void (*resolved_isr)(void)) {
    uint32_t* ram_vectors = (uint32_t*)SCB->VTOR;
    uint32_t vector_offset = (int32_t)irq_type + 16;
    
    // Write absolute address directly (safe because relocatable code has no GOT/r10 dependencies)
    ram_vectors[vector_offset] = (uint32_t)resolved_isr;
    NVIC_EnableIRQ(irq_type);
}
```

### Pattern C: Self-Contained Assembly Wrapper (Module-Side Trampoline)

If the module is a shared library (`.so`) but needs to register itself directly into a RAM-based vector table, the registered pointer **cannot** point to the C-coded ISR. It must point to a naked assembly wrapper inside the module.

This wrapper is responsible for determining the module's GOT base address, configuring `r10`, and executing the handler.

```assembly
/* Module Assembly Wrapper (handler.S) */
.syntax unified
.cpu cortex-m4
.thumb

.section .text.isr_wrapper, "ax", %progbits
.global my_module_isr_wrapper
.type my_module_isr_wrapper, %function

my_module_isr_wrapper:
    push {r4, lr}
    
    // Load the GOT base address relative to the PC
    ldr r4, =_GLOBAL_OFFSET_TABLE_
    mov r10, r4
    
    // Call the actual C handler
    bl my_module_isr_c_handler
    
    pop {r4, pc}
    .size my_module_isr_wrapper, .-my_module_isr_wrapper
```

---

### Pattern D: Dynamic RAM Vector Table Patching with Trampoline (Recommended for `scl::xip-static`)

If the dynamic module is compiled as a position-independent shared library (`-fPIC` which requires `r10` for local static/global access), but the system design requires dynamically registering the interrupt vector at runtime rather than hardcoding it in the ROM/Flash vector table, we can combine RAM vector table patching with a firmware-side trampoline.

#### How it works
1. **RAM Vector Table Initialization**: At boot, the main firmware copies the hardware vector table from Flash to RAM, aligning it to a 512-byte boundary (Cortex-M4 alignment requirement for a 128-word table), and points `SCB->VTOR` to the RAM copy.
2. **Dynamic Registration on Load**: When the static loader loads the module, we resolve the target handler and the library's `got_base`. We then dynamically patch the timer's vector slot in the RAM table with the address of a firmware-side trampoline function (`my_dynamic_tim2_handler`).
3. **Context-Saving Trampoline**: The trampoline preserves the caller's registers, loads the dynamic module's `got_base` into `r10`, calls the C handler, and restores `r10`.
4. **Dynamic Restoration on Unload**: When the module is unloaded, the vector slot in the RAM table is restored to `nullptr`.

#### Implementation Example
```c
/* Main Firmware Source */

// Align to 512 bytes for Cortex-M4 vector table requirements (16 exceptions + 86 interrupts)
__attribute__((aligned(512))) static uint32_t g_ram_vector_table[128];

void init_ram_vector_table(void) {
    uint32_t* flash_vectors = (uint32_t*)SCB->VTOR;
    for (int i = 0; i < 128; i++) {
        g_ram_vector_table[i] = flash_vectors[i];
    }
    __disable_irq();
    SCB->VTOR = (uint32_t)g_ram_vector_table;
    __DSB(); __ISB();
    __enable_irq();
}

void register_tim2_isr(void (*isr)(void)) {
    __disable_irq();
    g_ram_vector_table[TIM2_IRQn + 16] = (uint32_t)isr;
    __DSB(); __ISB();
    __enable_irq();
}

// Trampoline wrapper in main firmware
extern "C" void my_dynamic_tim2_handler(void) {
    if (TIM2->SR & TIM_SR_UIF) {
        TIM2->SR = ~TIM_SR_UIF;

        blink_leds[0].turn_on();
        delay_ms(100);
        blink_leds[0].turn_off();

        // Swap r10 with got_base and execute resolved handler
        if (g_fixture_systick_handler && g_fixture_got_base) {
            unsigned int toggle = 0;
            __asm__ volatile (
                "mov r4, r10\n"
                "mov r10, %1\n"
                "mov r0, #1\n"
                "blx %2\n"
                "mov %0, r0\n"
                "mov r10, r4\n"
                : "=r"(toggle)
                : "r"(g_fixture_got_base), "r"(g_fixture_systick_handler)
                : "r0", "r4", "lr", "memory"
            );
        }
    }
}
```

---

## 3. Preemptive Context Switching with Hard-FPU

When integrating a multitasking scheduler (such as an RTOS) in a Cortex-M4 environment with a hardware Floating-Point Unit (FPU), the context switcher must handle FPU register states and maintain strict alignment constraints.

### 3.1. FPU Register Stacking and `EXC_RETURN`
Upon exception entry, if the FPU was active (i.e. `CONTROL.FPCA == 1`), the Cortex-M4 hardware automatically stacks registers `s0-s15` and `FPSCR` in addition to the standard core registers. 
The software context switcher (`PendSV_Handler`) is responsible for saving and restoring the remaining callee-saved FPU registers (`s16-s31`) conditionally based on the `EXC_RETURN` value.

* **FPU Frame Detection**: Bit 4 of the Link Register (`lr` / `EXC_RETURN`) determines whether an FPU frame was stacked by the hardware:
  * `0`: FPU context active; FPU frame present on stack.
  * `1`: Standard context; FPU frame not present.
* **Conditional Stacking Assembly**:
  ```assembly
  // Check if FPU context was active (bit 4 of lr is 0)
  tst lr, #0x10
  it eq
  vstmdbeq r0!, {s16-s31}   @ Save FPU registers s16-s31
  ```

### 3.2. Enforcing Strict 8-byte Stack Alignment
Under the ARM Architecture Procedure Call Standard (AAPCS), the stack pointer must be aligned to an **8-byte boundary** at all external function calls. 
* **The Problem**: Pushing the core registers `r4-r11, lr` (9 registers = 36 bytes) shifts the stack pointer by a value that is not a multiple of 8, breaking alignment.
* **The Solution**: We include a padding register (e.g. `r3`) in the software stacking list, expanding the list to 10 registers (`r3-r11, lr` = 40 bytes), which preserves 8-byte alignment.
  ```assembly
  stmdb r0!, {r3-r11, lr}   @ 10 registers (40 bytes), maintains 8-byte alignment
  ```

### 3.3. Dynamic Stack-Selection Context Switcher (MSP / PSP Isolation)
In a hybrid multitasking system, we can configure Task 0 (the main firmware thread) to execute on the Main Stack Pointer (`MSP`) to support debuggers and standard libraries, while Task 1 (e.g., a background LED task) executes on an isolated Process Stack Pointer (`PSP`).

To do this, the context switcher must dynamically load/store from the active stack based on bit 2 of `EXC_RETURN` (which indicates whether `MSP` or `PSP` was active):
* `0`: Interrupted context was using `MSP`.
* `1`: Interrupted context was using `PSP`.

```assembly
/* Dynamic Stack-Selection Context Switcher */
extern "C" __attribute__((naked)) void PendSV_Handler(void) {
    __asm__ volatile (
        "cpsid i\n"

        // 1. Check active stack pointer (bit 2 of EXC_RETURN in lr)
        "tst lr, #4\n"
        "ite ne\n"
        "mrsne r0, psp\n"               // If PSP was active, load into r0
        "mrseq r0, msp\n"               // If MSP was active, load into r0

        // 2. Conditionally stack FPU registers
        "tst lr, #0x10\n"
        "it eq\n"
        "vstmdbeq r0!, {s16-s31}\n"

        // 3. Stack core registers + EXC_RETURN (including r3 for 8-byte alignment)
        "stmdb r0!, {r3-r11, lr}\n"

        // 4. Save SP back to current TCB
        "ldr r1, =g_current_task\n"
        "ldr r1, [r1]\n"
        "ldr r2, =g_tasks\n"
        "lsl r1, r1, #3\n"
        "str r0, [r2, r1]\n"

        // 5. Select next task
        "bl select_next_task\n"

        // Set g_current_task = r0
        "ldr r1, =g_current_task\n"
        "str r0, [r1]\n"

        // 6. Load new task SP
        "ldr r2, =g_tasks\n"
        "lsl r0, r0, #3\n"
        "ldr r0, [r2, r0]\n"

        // 7. Pop core registers
        "ldmia r0!, {r3-r11, lr}\n"

        // 8. Conditionally pop FPU registers
        "tst lr, #0x10\n"
        "it eq\n"
        "vldmiaeq r0!, {s16-s31}\n"

        // 9. Restore stack pointer to the correct register (MSP or PSP) based on bit 2 of new lr
        "tst lr, #4\n"
        "ite ne\n"
        "msrne psp, r0\n"
        "msreq msp, r0\n"

        "cpsie i\n"
        "bx lr\n"
        : : : "memory"
    );
}
```

---

## 4. Debugging & Timer Interrupts under Semihosting

If the application uses **semihosting** (such as printing log output via `trace_printf` over SWD/JTAG), debugging interrupts becomes a challenge because of how the CPU core behaves during debugger halts.

### 4.1. The SysTick Debug Freeze

The SysTick timer is physically integrated directly inside the ARM Cortex-M processor core. Because of this integration, **whenever the CPU core is halted by a debugger, the SysTick timer freezes instantly and stops counting.**

This behavior causes a severe conflict when the application utilizes **semihosting** (such as printing test logs to the host PC via `trace_printf()`):

1. **How Semihosting Halts the Core**:
   Every time the firmware calls `trace_printf()`, it executes a `BKPT` (breakpoint) instruction. The processor immediately halts. The hardware debugger (e.g., OpenOCD/GDB via JTAG/SWD) detects the halt, reads the log text out of the microcontroller's RAM, prints it to your PC's command line, and then instructs the CPU to resume execution.
   
2. **The Time Inflation Discrepancy**:
   Because sending characters to a PC terminal over a debug probe is slow, each print statement pauses the CPU for **100 to 200 milliseconds** of real-world desk time. However, the CPU active execution time *between* these print statements (the actual test math) is extremely fast, taking only **a few microseconds**.

3. **Why the Interrupt Never Fires**:
   * **Real-world time elapsed on your desk**: 10 seconds (due to 50 print statements pausing the core for 200ms each).
   * **Active CPU time counted by SysTick**: 0.0005 seconds (50 active test blocks of 10 microseconds each, as SysTick was frozen during all print pauses).
   
   Since SysTick only registers 0.0005 seconds of elapsed time during the entire test suite run, the timer is nowhere near its 1.0-second limit. Therefore, the SysTick interrupt never fires, and the LED remains completely dark during the tests.
   
   Only when all prints finish and the CPU enters the final continuous loop (`for (;;) { wfi; }`) does SysTick finally accumulate enough active cycles to reach 1.0 second and begin blinking the LED.

### 4.2. The APB Peripheral Timer Advantage (`TIM2`/`TIM3`)
To make the LED blink during semihosting execution, we must use a **Hardware Timer** (like `TIM2`) instead of SysTick.
* **Continuous Bus Clocking**: Hardware timers are peripheral modules running on the APB bus. By default, they continue to count even when the CPU core is halted by a debugger.
* **Instant Interrupt Execution**: While the core is halted, `TIM2` continues counting. Once the debugger resumes the CPU core, the timer immediately asserts its interrupt request, triggering `TIM2_IRQHandler` to execute and blink the LED during active test windows.

```cpp
/* Configure TIM2 to overflow every 1.0 second */
void timer_start(void) {
    // 1. Enable TIM2 clock on APB1
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
    __asm__ volatile ("dsb\n isb\n");

    // 2. Fetch APB1 clock frequency and calculate timer clock frequency
    std::uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();
    std::uint32_t tim_clk = pclk1;
    if ((RCC->CFGR & RCC_CFGR_PPRE1) != 0) {
        tim_clk = 2 * pclk1;
    }

    // 3. Configure Prescaler to tick at 10 kHz, and Auto-reload for 1Hz overflow
    TIM2->PSC = (tim_clk / 10000) - 1;
    TIM2->ARR = 10000 - 1;

    // 4. Clear flag, enable interrupt, enable NVIC, and start timer
    TIM2->CNT = 0;
    TIM2->SR = ~TIM_SR_UIF;
    TIM2->DIER |= TIM_DIER_UIE;
    NVIC_EnableIRQ(TIM2_IRQn);
    TIM2->CR1 |= TIM_CR1_CEN;
}
```

### 4.3. The 24-bit SysTick Reload Register Limit
When using SysTick, the reload register is only **24 bits wide**, allowing a maximum value of `16,777,215`.
* **The Failure**: Attempting to load `SystemCoreClock` (e.g., `100,000,000` or `168,000,000` for a 1-second interval) overflows the register, causing `SysTick_Config` to immediately fail and abort configuration. The timer then falls back to the default high-frequency 1000 Hz tick rate configured by the HAL.
* **The Solution**: If SysTick is required for low-frequency intervals, configure it to run at a high frequency (like **1000 Hz**), and use volatile tick counters inside the ISR to implement low-frequency actions:
  ```cpp
  extern "C" void SysTick_Handler(void) {
      g_systick_counter = g_systick_counter + 1;
      if (g_systick_counter >= 1000) { // 1000ms = 1.0 second
          g_systick_counter = 0;
          blink_leds[0].turn_on();
          g_led_off_counter = 100;    // LED on for 100ms
      }
      
      if (g_led_off_counter > 0) {
          g_led_off_counter = g_led_off_counter - 1;
          if (g_led_off_counter == 0) {
              blink_leds[0].turn_off();
          }
      }
  }
  ```

### 4.4. Early Boot Starvation Guard
At boot, the HAL initializes SysTick to fire at 1000 Hz. If the custom `SysTick_Handler` uses a busy-wait delay (like `delay_ms(100)`) inside the ISR *before* the SysTick timer is reconfigured to a lower frequency, the handler will take 100ms to complete and will immediately re-trigger, completely starving the main thread.
* **Resolution**: Keep a `volatile bool g_blink_enabled = false` status flag. Guard the ISR with this flag, and only set it to `true` after reconfiguring the timer to a safe frequency.

### 4.5. SysTick-Dependent Delay Lockup inside High-Priority ISRs
A common pitfall in embedded systems is calling a platform delay helper (like `delay_ms(100)`) from inside a hardware timer interrupt handler (such as a `TIM2` ISR).
* **The Failure**: Typical platform delay loops (e.g., in `board.c`) spin on a global millisecond counter (`s_millis`) that is updated by the **SysTick exception handler**:
  ```c
  void delay_ms(uint32_t ms) {
      uint32_t start = s_millis;
      while ((s_millis - start) < ms) { /* spin */ }
  }
  ```
  Since the processor is in Handler Mode executing the high-priority `TIM2_IRQHandler`, the `SysTick` exception (which usually runs at equal or lower priority) is blocked from executing. As a result, `s_millis` never increments, and the loop spins forever, locking up the CPU with the LED permanently on.
* **The Solution**: Avoid calling any timer-tick-dependent delay functions inside an ISR context. Instead, implement a freestanding instruction-based busy loop (`local_delay_ms`) that burns CPU cycles directly without relying on peripheral interrupts:
  ```cpp
  inline void local_delay_ms(std::uint32_t ms) {
      // 16 MHz HSI clock: 5333 loop cycles per millisecond
      std::uint32_t count = 5333 * ms;
      __asm__ volatile (
          "1:\n"
          "subs %0, %0, #1\n"
          "bne 1b\n"
          : "+r"(count)
          :
          : "cc"
      );
  }
  ```

---

## 5. Summary of Architectural Design Choices

| Metric | Main Firmware Trampoline | Relocatable Objects Vector Patch | Assembly Module Wrapper | TIM2 Hardware Timer (Host-Debug) |
| :--- | :--- | :--- | :--- | :--- |
| **Interrupt Latency** | Low (Minimal trampoline overhead) | Zero (Direct hardware branch) | Low (Wrapper call overhead) | Low (ISR entrance overhead) |
| **GOT Base (`r10`)** | Handled by Firmware Trampoline | Not Used (Absolute symbols only) | Handled by Module Assembly | Handled by ISR wrapper if PIC |
| **Debugger Stability** | Frozen during BKPT halts | Frozen during BKPT halts | Frozen during BKPT halts | **Active during BKPT halts** |
| **C/C++ Code Eligibility** | Yes (Standard PIC function) | Yes (Standard absolute function) | Yes (Wrapped via ASM entry) | Yes (Direct handler routing) |
| **Implementation Risk** | Low (Centralized in main firmware) | Medium (Loader maps symbols) | High (Custom assembly needed) | Low (Standard timer module) |
