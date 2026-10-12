<p align="center">
  <h1 align="center">MimiPS1</h1>
  <p align="center">
    <strong>Ahead-of-Time (AOT) Static Recompiler & Native macOS Runtime for PlayStation 1 (MIPS R3000A) on Apple Silicon</strong>
  </p>
  <p align="center">
    <a href="https://github.com/wlfrotaneto/MimiPS1/actions"><img src="https://img.shields.io/badge/build-passing-brightgreen?style=flat-square" alt="Build"></a>
    <a href="https://developer.apple.com/metal/"><img src="https://img.shields.io/badge/graphics-Metal%20MSL-blue?style=flat-square" alt="Metal"></a>
    <a href="https://www.apple.com/silicon/"><img src="https://img.shields.io/badge/platform-Apple%20Silicon%20(ARM64)-orange?style=flat-square" alt="Platform"></a>
    <a href="https://opensource.org/licenses/MIT"><img src="https://img.shields.io/badge/license-MIT-purple?style=flat-square" alt="License"></a>
    <a href="https://github.com/wlfrotaneto/MimiPS1/stargazers"><img src="https://img.shields.io/github/stars/wlfrotaneto/MimiPS1?style=flat-square" alt="Stars"></a>
  </p>
</p>

---

## Overview

**MimiPS1** is a next-generation Ahead-of-Time (AOT) static recompiler and native runtime engine designed specifically for macOS on Apple Silicon.

Following the modern wave of static recompilation architectures (such as *N64Recomp* and native console binary transpilators), MimiPS1 disassembles PlayStation 1 (MIPS R3000A) binaries, reconstructs full Control Flow Graphs (CFGs), resolves branch delay slots, and transpiles original machine code directly into optimized C. The recompiled code is then compiled via Apple Clang (`-O3 -flto`) into a native ARM64 Mach-O executable, linked against a bespoke Metal & CoreAudio hardware runtime, and packaged directly into a standalone macOS `.app` bundle and compressed `.dmg` installer.

```
┌─────────────────────────────────┐
│  Original PS1 Disc / Binary     │ (SCUS_*.00, .EXE, .BIN, .ISO)
└────────────────┬────────────────┘
                 │
                 ▼
┌─────────────────────────────────┐
│     MimiPS1 Static Recompiler   │ (MIPS-I Disassembly & CFG Function Recovery)
└────────────────┬────────────────┘
                 │
                 ▼
┌─────────────────────────────────┐
│    Structured C99 Generation    │ (Delay slot resolution & branch folding)
└────────────────┬────────────────┘
                 │
                 ▼
┌─────────────────────────────────┐
│   Apple Clang Compiler (-O3)    │ (LTO + Native ARM64 Vector Instructions)
└────────────────┬────────────────┘
                 │
                 ▼
┌─────────────────────────────────┐
│   Native macOS App Bundle       │ (Metal MSL + CoreAudio + GameController)
│   Distributed as Compressed DMG │
└─────────────────────────────────┘
```

---

## Architectural Highlights

### 🚀 Static Recompilation vs. Dynamic Emulation

| Feature | Traditional Emulator (JIT / Interpreter) | MimiPS1 Static Recompiler |
| :--- | :--- | :--- |
| **Execution Model** | Dynamic translation in memory (JIT cache & page faults) | **Ahead-of-Time compiled directly to native ARM64** |
| **CPU Overhead** | 5%–25% (JIT translation cache management & dispatch loops) | **< 0.5% CPU utilization** (Direct Mach-O execution) |
| **Input Latency** | 1–3 frame latency due to interpreter frame queues | **Sub-millisecond instant response** |
| **Distribution** | Requires external emulator app, BIOS dumps, & separate ROM files | **Single standalone `.dmg` installer** |
| **External Dependencies**| Heavy external dependencies (SDL2, Qt, third-party DLLs) | **Zero external dependencies** (Pure Cocoa, Metal & CoreAudio) |

### 🛠️ Subsystems Architecture

* **MIPS R3000A Recompiler (`recompiler/`):**
  * Instruction decoder handling complete MIPS-I instruction set, Coprocessor 0/2 operations, and delay slot semantics.
  * Recursive function entry-point analysis and basic block boundary reconstruction.
  * Register mapping with direct virtual register assignments optimized for Clang register allocators.

* **Metal MSL Graphics Pipeline (`runtime/src/platform/macos_metal.m`):**
  * Emulates the 1024×512 PlayStation 1 VRAM directly mapped to native Metal textures.
  * Custom Metal Shading Language (MSL) shaders handle aspect ratio correction, dynamic resolution scaling, and high-DPI Retina rasterization.
  * Native 60 FPS synchronization aligned to macOS display refresh cycles (ProMotion ready).

* **CoreAudio SPU Synthesis (`runtime/src/platform/macos_audio.m`):**
  * 24-channel audio synthesizer with low-latency `AudioQueue` streaming.
  * Native sample rate conversion and mixer buffering without third-party audio abstractions.

* **CD-ROM Engine (`runtime/src/mimips1_cdrom.c`):**
  * Dual-format sector engine handling standard 2048-byte ISO sectors and raw 2352-byte disc images (Mode 2 Form 1 with sync pattern detection).
  * Direct DMA 3 data streaming and ISO9660 volume descriptor parser.

* **High-Level BIOS Engine (`runtime/src/mimips1_bios.c`):**
  * HLE implementation of critical Sony kernel syscall tables (`A0`, `B0`, `C0`).
  * Eliminates the legal and logistical barrier of requiring proprietary PS1 BIOS dumps.

* **Controller Input (`runtime/src/platform/macos_main.m`):**
  * Native Apple `GameController` framework integration with automatic mapping for PlayStation DualSense (PS5), DualShock 4, Xbox Wireless, and Nintendo Switch controllers.

---

## Repository Structure

```
MimiPS1/
├── CMakeLists.txt                # Root CMake build configuration
├── Makefile                      # Convenient CLI automation targets
├── recompiler/                   # Ahead-of-Time Static Recompiler engine
│   ├── include/                  # MIPS disassembly, CFG analysis & C emitter headers
│   └── src/                      # Disassembler, CFG recovery and C transpiler
├── runtime/                      # Native macOS / Apple Silicon runtime engine
│   ├── include/                  # Subsystem interfaces (GPU, GTE, SPU, CD-ROM, BIOS)
│   └── src/
│       ├── platform/             # Metal shaders, CoreAudio driver, Cocoa windowing
│       └── mimips1_*.c           # Memory bus, DMA, timers, and CPU register state
├── tools/                        # Automation & deployment pipelines
│   ├── mimips1_pipeline.py       # End-to-end recompilation & packaging pipeline
│   ├── build_app_bundle.sh       # macOS .app bundle constructor
│   └── package_dmg.sh            # Compressed UDZO .dmg packaging utility
└── samples/                      # Validation test suites and homebrew samples
    └── cube_demo/                # Standalone PS-X test ROM with 3D geometry
```

---

## Getting Started

### Prerequisites

* macOS 12.0 (Monterey) or newer (Apple Silicon M1/M2/M3/M4)
* Xcode Command Line Tools (`xcode-select --install`)
* CMake 3.20+
* Python 3.9+

### Building the Toolchain

Clone the repository and build the static recompiler:

```bash
git clone https://github.com/wlfrotaneto/MimiPS1.git
cd MimiPS1
cmake -B build -S .
cmake --build build --target mimips1-recomp
```

---

## Usage

### Complete End-to-End Pipeline

The automated pipeline handles executable extraction, MIPS disassembly, C code generation, Clang compilation, `.app` bundle construction, and `.dmg` packaging with a single command:

```bash
# Recompile any PS1 executable or disc image (.EXE, .BIN, .ISO)
python3 tools/mimips1_pipeline.py pipeline path/to/game.bin --name "GameTitle"
```

#### Self-Contained Distribution (Embedding Disc Assets)

To package a commercial disc image directly inside the resulting macOS app bundle (enabling drag-and-drop distribution without requiring users to supply external ROM files):

```bash
python3 tools/mimips1_pipeline.py pipeline path/to/game.bin --name "GameTitle" --embed-disc
```

Once the pipeline completes:
* **Native App:** `build/GameTitle.app`
* **Distribution DMG:** `dist/GameTitle-AppleSilicon.dmg`

### Running the Sample Demo

To verify your build environment using the included 3D hardware test demo:

```bash
python3 tools/mimips1_pipeline.py pipeline samples/cube_demo/TRIANGLE_DEMO.EXE --name "TriangleDemo"
open build/TriangleDemo.app
```

---

## Current Project Status

> [!NOTE]
> **MimiPS1 is currently in active research & development.**

* **Homebrew & Technical Demos:** Fully functional. Standalone executables such as `TriangleDemo.EXE` recompile cleanly, render in real-time via Metal at 60 FPS, process input, and package into standalone macOS applications.
* **AOT Static Recompiler:** Functional and robust. Converts MIPS R3000A machine code into clean, optimizable C99, resolves branch delay slots, reconstructs Control Flow Graphs (CFGs), and supports Psy-Q function prologue detection.
* **Commercial Game Support (WIP / Experimental):** Under active development. Commercial titles utilizing the Psy-Q SDK (such as *Cotton Original* and *Crash Bandicoot*) successfully disassemble, link against the native runtime, boot, and enter their main game loops. However, they are currently subject to synchronization stalls prior to their first display frame swap due to hardware interrupt timing.
* **Current Focus:** Unifying CPU execution, timers, CD-ROM status events, and hardware IRQs into a single-threaded cycle-synchronized loop (eliminating host-thread race conditions between the Metal presentation layer and the recompiled game loop).

---

## Technical Roadmap

### Architecture & Synchronization (Active)
- [x] MIPS-I R3000A instruction decoding & basic block partitioning
- [x] Function recovery, delay slot resolution, and CFG analysis
- [x] Metal hardware-accelerated rasterization & 1024×512 VRAM pipeline
- [x] CoreAudio SPU streaming engine
- [x] BIOS HLE support for kernel vector tables (`A0`, `B0` Rev 1/2, `C0`)
- [x] Dual-format disc sector parser (2048-byte ISO & 2352-byte RAW Mode 2 Form 1)
- [x] macOS `.app` bundle builder & compressed UDZO `.dmg` packaging
- [ ] **Single-threaded cycle-budgeted event scheduler:** Move IRQ delivery and timer ticks out of the Metal rendering thread into synchronous execution points within the CPU thread to ensure deterministic Psy-Q driver operation.
- [ ] Full CD-ROM controller state machine timing (CdlReadN/CdlSeekL sub-response delays)
- [ ] ADPCM voice ADSR envelope curves & hardware reverberation buffers

### Future Multiplatform Vision
- [ ] **Windows (x86_64) Platform Backend:** Leverage MimiPS1's pure C11 core with a modular SDL2/SDL3 platform layer (replacing Cocoa/Metal/CoreAudio with DirectX/Vulkan/OpenGL and miniaudio).

## License

This project is licensed under the [MIT License](LICENSE).

---

<p align="center">
  <sub>Built for macOS and Apple Silicon. PlayStation is a registered trademark of Sony Interactive Entertainment Inc. This project is an independent research endeavor and is not affiliated with or endorsed by Sony.</sub>
</p>
