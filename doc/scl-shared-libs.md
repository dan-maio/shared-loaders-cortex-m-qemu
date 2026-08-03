Here's a consolidated and polished version of the entire document incorporating all the requested changes.

# Refactoring Plan: Cortex-M Source Code Libraries (SCL)

## Objective

Analyze in detail all projects located in:

```text
/home/dan/Downloads/f411re-libs/
```

excluding the **Rust** and **Zig** projects.

The analysis should focus on the projects that implement **shared library loading** on Cortex-M microcontrollers, specifically those tested on the **STM32F411CE Nucleo** board.

These projects implement several loading mechanisms (static loading, dynamic loading, overlays, etc.) together with different symbol resolution techniques that mimic part of the functionality provided by the Linux dynamic loader (`ld.so`/`dlopen`).

The goal is to identify reusable components, eliminate duplicated code, and reorganize the existing monolithic implementations into a modular **Source Code library (SCL)**.

---

# Goal

Create a new project inside `f411re-libs` named:

```text
cortex-m-scl
```

(SCL = **Source Code Libraries**)

The SCL project will contain reusable source code only. It will not generate executable firmware images or standalone applications. Instead, it will provide configurable libraries that can be integrated into multiple Cortex-M applications using CMake.

---

# Proposed Directory Structure

```text


── shared-libs-loaders/                          # Shared library infrastructure
   ├── include/
   │   ├── static/                      # Static loader public API
   │   ├── dynamic/                     # Dynamic loader public API
   │   └── overlay/                     # Overlay loader public API
   │
   ├── src/
   │   ├── static/                      # Static loader implementation
   │   ├── dynamic/                     # Dynamic loader implementation
   │   └── overlay/                     # Overlay loader implementation
   │
   ├── tests/
   │   ├── static/
   │   	   ├── include/
	   ├── src/
	   ├── CMakeLists.txt
   │   ├── dynamic/
   │   └── overlay/
       └── CMakeLists.txt
   │
   ├── CMakeLists.txt
   └── README.md


```

---

# Design Requirements

The `shared-lib` module contains all reusable functionality required to implement shared library loading on Cortex-M.

Loader-specific code is organized into three independent modules:

* **static** — static shared-library loader.
* **dynamic** — dynamic shared-library loader.
* **overlay** — overlay loader.

Each module exposes its own public API under `include/` and its implementation under `src/`.

Whenever functionality is shared between multiple loader implementations, it should be implemented only once and reused rather than duplicated.

The library must remain independent of:

* board support packages;
* startup code;
* linker scripts;
* storage devices;
* filesystems;
* debugging infrastructure;
* operating systems;
* runtime environments.

---

# Application Responsibilities

The SCL libraries provide only the reusable loader functionality.

The final application is responsible for providing all platform-specific services, including:

* board initialization;
* clock configuration;
* memory initialization;
* filesystem implementation;
* storage backend access;
* logging and debugging facilities;
* memory allocation hooks (when configurable);
* application configuration.

Examples of supported storage backends include, but in responsability of main app:

* Semihosting
* FlatFS
* SD card
* Internal Flash
* External Flash
* Any custom storage implementation

The SCL libraries must never assume a particular storage backend or execution environment.

---

# Configuration

The SCL project must be fully configurable through:

* CMake options;
* compile definitions;
* include directories;
* optional feature selection.

Applications should enable only the loader components they require.

Optional functionality should be controlled through compile-time configuration rather than source modifications.

---

# Build System

The entire project is built using **CMake**.

The SCL project produces reusable library targets only.

It must not generate executable firmware.

Applications integrate the project using:

```cmake
add_subdirectory(cortex-m-scl)
```

The application supplies:

* compile definitions;
* include paths;
* platform-specific implementations;
* filesystem drivers;
* board support;
* linker configuration.

Each application links only the required libraries.

---

# Tests

The `tests` directory contains unit tests and integration tests organized by loader type:

* `tests/static`
* `tests/dynamic`
* `tests/overlay`

Whenever possible, tests should execute on the host platform.

Target-specific integration tests for the STM32F411CE Nucleo board should reuse the same test sources through thin platform-specific wrappers.

---

# Migration Plan

Produce a detailed analysis of the existing projects and prepare a migration plan for transforming the current monolithic implementations into reusable source libraries.

The analysis should identify:

0. Find API/configurations  used by main app to initialize/use slared libraries
1. Functionality shared by multiple projects.
2. Static loader implementation.
3. Dynamic loader implementation.
4. Overlay loader implementation.
5. Symbol resolution mechanisms.
6. Relocation handling.
7. Export/import symbol management.
8. ELF parsing code (where applicable).
9. Filesystem abstraction points.
10. Storage abstraction points.
11. Platform-dependent code.
12. Board-specific code.
13. Public APIs.
14. Internal/private APIs.
15. Header reorganization.
16. Source file reorganization.
17. CMake target organization.
18. Existing code duplication.
19. Opportunities to simplify the architecture.
20. A phased migration strategy that preserves the functionality of the current test applications throughout the refactoring process.

---

# Expected result

The final architecture should provide:

* a reusable **Source Code library (SCL)** for Cortex-M shared-library loading;
* a modular and maintainable codebase;
* minimal code duplication;
* clean separation between reusable loader code and application-specific code;
* configurable features selected at compile time;
* reusable CMake library targets;
* support for multiple storage backends without modifying the core library;
* straightforward integration into existing and future applications;
* preservation of the existing test applications as independent consumers of the SCL libraries.

The existing example projects should be progressively refactored to consume the new SCL libraries, becoming reference applications that demonstrate different loader configurations rather than containing their own independent implementations.
