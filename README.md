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
│  Original PS1 Disc / Binary     │ (SCUS_*.00, .EXE, .BIN, .ISO, .CUE)
└────────────────┬────────────────┘
                 │
                 ▼
┌─────────────────────────────────┐
│     MimiPS1 Static Recompiler   │ (MIPS-I Disassembly & CFG Function Recovery)
└────────────────┬────────────────┘
                 │
                 ▼
┌─────────────────────────────────┐
│    Structured C99 Generation    │ (Delay slot resolution, branch folding & GTE)
└────────────────┬────────────────┘
                 │
                 ▼
┌─────────────────────────────────┐
│   Apple Clang Compiler (-O3)    │ (LTO + Native ARM64 Vector Instructions)
└────────────────┬────────────────┘
                 │
                 ▼
┌─────────────────────────────────┐
│   Native macOS App Bundle       │ (Metal MSL + CoreAudio + GameController + Auto-Icon)
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
| **Distribution** | Requires external emulator app, BIOS dumps, & separate ROM files | **Single standalone `.app` and `.dmg` installer** |
| **Game Icons** | Generic or non-existent in OS file manager | **Auto-extracted PS1 Memory Card pixel art or official HD Boxart** |
| **External Dependencies**| Heavy external dependencies (SDL2, Qt, third-party DLLs) | **Zero external dependencies** (Pure Cocoa, Metal & CoreAudio) |

### 🛠️ Subsystems Architecture

* **MIPS R3000A Recompiler (`recompiler/`):**
  * Instruction decoder handling complete MIPS-I instruction set, Coprocessor 0/2 operations, and delay slot semantics.
  * Recursive function entry-point analysis and basic block boundary reconstruction with configurable CFG safety limits.
  * Preserves out-of-function conditional calls (`BLTZAL`, `BGEZAL`) without corrupting return control flow.
  * Register mapping with direct virtual register assignments optimized for Clang register allocators.

* **Geometry Transformation Engine (GTE) (`runtime/src/mimips1_gte.c`):**
  * Cycle-accurate vector transformations (`RTPT`, `RTPS`, `MVMVA`, `NCLIP`, `AVSZ3`, `AVSZ4`).
  * Fixed-point 12-bit vector pipeline with dedicated hardware accuracy test suite (`tests/gte_test.c`).

* **Metal MSL Graphics Pipeline (`runtime/src/platform/macos_metal.m` & `runtime/src/mimips1_gpu.c`):**
  * Emulates the 1024×512 PlayStation 1 VRAM directly mapped to native Metal textures.
  * Full GPU command execution (`GP0` polygon rendering, line drawing, sprites/rectangles, texture sampling, CLUT palettes).
  * Color modulation with saturation safeguards (`(r * tr) >> 7`) and drop shadow rendering (`(0, 0, 0)` vertex modulations).
  * Texture window wrapping (`GP0(0xE2)`), 240p/480i overscan clipping, and VRAM-to-VRAM copy masking (`GP0(0x80..0x9F)`).
  * Hardware-accurate PS1 15-bit color semi-transparency handling: `0x8000` (STP=1, RGB=0) treated as opaque black, eliminating transparent holes in menu textures.
  * Native 60 FPS synchronization aligned to macOS display refresh cycles (ProMotion ready).

* **CoreAudio SPU Synthesis (`runtime/src/platform/macos_audio.m` & `runtime/src/mimips1_spu.c`):**
  * 24-channel SPU: ADPCM decoding with Catmull-Rom interpolation, hardware ADSR (linear/exponential rates), pitch modulation, noise, ENDX, mute/enable, low-latency `AudioQueue` streaming.
  * XA-ADPCM (4-bit mono/stereo, 37.8/18.9 kHz) decoded by the CD controller and resampled into the mix. Reverb is not implemented yet.

* **CD-ROM & DMA Controller (`runtime/src/mimips1_cdrom.c` & `runtime/src/mimips1_dma.c`):**
  * Dual-format sector engine handling standard 2048-byte ISO sectors and raw 2352-byte disc images (Mode 2 Form 1).
  * Recursive ISO9660 directory lookup (up to depth 4) with device path prefix stripping (`cdrom:`, `sim:`, `c:\`), enabling games with multi-level folder hierarchies (such as Castlevania) to seamlessly load subfolder overlays (`BIN/`, `BOSS/`, `ST/`).
  * Paced sector delivery (`16384` cycles) with in-flight sector protection and safe `BFRD` latching to prevent sector dropping during DMA3 streaming.
  * Exact `GetlocL` (0x10) and `GetlocP` (0x11) position tracking via `last_read_lba`.
  * Multi-channel DMA transfers (GPU Linked-List Mode with bit 23 boundary check, immediate block mode vs slice mode word count differentiation).

* **MIPS Interpreter + Real BIOS Boot (`runtime/src/mimips1_interp.c`):**
  * Reference R3000A engine: branch and load delay slots (with LWL/LWR forwarding), COP0 exceptions (EPC/BD/SR stack), hardware interrupts, overflow/address-error traps, cache isolation.
  * With an official BIOS (`assets/bios.bin`) and a disc, the emulator boots at the reset vector and the **real BIOS loads the game from the disc**. The HLE kernel remains as a fallback (`MIMIPS1_HLE=1` or no BIOS).
  * Recompiled functions are entered natively from the interpreter on taken jumps/calls, but only after the boot executable's RAM image matches the original. Stores/DMA that change recompiled instructions invalidate just that function. Native code is never entered during a kernel exception.
  * Code that was never recompiled (overlays, generated code) simply runs in the interpreter.

* **Event-driven timing (`runtime/src/mimips1_runtime.c`):** deterministic cycle counter; hardware only latches `I_STAT`, handlers run at safe points honouring `I_MASK` and `SR`. Wall-clock time is used only for 60 Hz frame pacing.

* **MDEC (`runtime/src/mimips1_mdec.c`):** full macroblock decoder (run-length, dequantisation, IDCT, YCbCr→RGB, 15/24-bit) with request-driven DMA0/DMA1.

* **Memory Card Manager (`runtime/src/mimips1_card.c`):**
  * Dual 128KB memory card persistence in `~/Library/Application Support/MimiPS1/memcards/`.

* **Automated Game Icon & Isolated Pipeline (`tools/mimips1_pipeline.py`):**
  * **Isolated Builds:** Per-game intermediate build directories (`build/recompiled_<slug>/`) preventing multi-game artifact collisions.
  * **Tier 1:** Native disc extraction of PS1 Memory Card icons (`*ICON*.TIM`) with 16-color CLUT decoding and nearest-neighbor pixel art scaling.
  * **Tier 2:** Automatic online retrieval of official high-resolution boxart (512×512 / 1024×1024) via Libretro/GameTDB repositories.
  * **Tier 3:** Built-in conversion via macOS `sips` and `iconutil` directly into standalone `.icns` application icons.

---

## Repository Structure

```
MimiPS1/
├── CMakeLists.txt                # Root CMake build configuration
├── Makefile                      # Convenient CLI automation targets
├── assets/                       # Embedded runtime assets, BIOS and icon cache
│   ├── bios.bin                  # Official 512KB PS1 BIOS image (auto-detected)
│   ├── AppIcon.icns              # Generic controller fallback icon
│   └── icons/                    # Auto-generated & cached game-specific .icns icons
├── recompiler/                   # Ahead-of-Time Static Recompiler engine
│   ├── include/                  # MIPS disassembly, CFG analysis & C emitter headers
│   └── src/                      # Disassembler, CFG recovery and C transpiler
├── runtime/                      # Native macOS / Apple Silicon runtime engine
│   ├── include/                  # Subsystem interfaces (GPU, GTE, SPU, CD-ROM, BIOS, Card)
│   └── src/
│       ├── platform/             # Metal shaders, CoreAudio driver, Cocoa windowing
│       └── mimips1_*.c           # Memory bus, DMA, timers, and CPU register state
├── tests/                        # Accuracy test suites
│   └── gte_test.c                # GTE hardware accuracy validation suite
├── tools/                        # Automation & deployment pipelines
│   ├── mimips1_pipeline.py       # Zero-configuration end-to-end recompilation pipeline
│   ├── build_app_bundle.sh       # macOS .app bundle constructor with icon integration
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

Clone the repository and build the static recompiler and test suite:

```bash
git clone https://github.com/wlfrotaneto/MimiPS1.git
cd MimiPS1
cmake -B build -S .
cmake --build build -j
```

Run the hardware accuracy validation suite:

```bash
./build/gte_test
```

---

## Usage

### Zero-Configuration Recompilation

The pipeline is completely automated. You only need to pass the path to your disc image or executable — the game title, disc embedding, BIOS, and custom icon are resolved automatically:

```bash
# Recompile any game with a single command:
python3 tools/mimips1_pipeline.py pipeline "/path/to/Crash Bandicoot [SCUS 94900].bin"
```

The pipeline will:
1. Detect and sanitize the game title (e.g., `Crash Bandicoot`).
2. Detect the BIOS from `assets/bios.bin` or `~/Downloads`.
3. Extract the boot executable (`SCUS_949.00`).
4. Recompile MIPS code to C and compile ARM64 native binary with Clang (`-O3 -flto`).
5. Resolve the game icon (disc extraction or official HD boxart).
6. Build `build/Crash Bandicoot.app` and `dist/Crash Bandicoot-AppleSilicon.dmg`.

### Useful Command Options

```bash
# Fast build (skips DMG compression):
python3 tools/mimips1_pipeline.py pipeline "path/to/game.bin" --no-dmg

# Install directly into /Applications:
python3 tools/mimips1_pipeline.py pipeline "path/to/game.bin" --install

# Custom application name or icon override:
python3 tools/mimips1_pipeline.py pipeline "path/to/game.bin" --name "MyGame" --icon "cover.png"
```

### Launching Recompiled Games

```bash
open "build/Crash Bandicoot.app"
```

---

## Current Project Status

| Title / Target | Serial | Boot | Status |
| :--- | :--- | :---: | :--- |
| **Triangle Demo (Homebrew)** | N/A | HLE | **Playable** |
| **Crash Bandicoot** | `SCUS-94900` | Real BIOS | **Logos (with the dog), title with menu, island map, level loading and gameplay.** Memory card detected (empty). SPU audio |
| **Cotton Original** | `SLPM-86461` | Real BIOS | **Title screen, attract-mode gameplay and rankings.** SPU audio |
| **Castlevania: Symphony of the Night** | `SLUS-00067` | Real BIOS | **Opening FMV (24-bit, with XA audio), title, file select, name entry and the opening scene.** |

--- | :--- | :---: | :---: | :---: | :--- |
| **Triangle Demo (Homebrew)** | N/A | ✅ Clean | ✅ Metal 60 FPS | N/A | **Fully Playable** |
| **Crash Bandicoot** | `SCUS-94900` | ✅ Clean (820 funcs) | 🟡 Intro & 3D Title Scene | ✅ Audio Streaming | **Boots through Intro & Title Screen** (Naughty Dog banner, 3D doghouse, zero visual artifacts, audio streaming) |
| **Cotton Original** | `SLPM-86461` | ✅ Clean (2133 funcs) | 🟡 Kernel & IRQ inited | 🟡 In progress | **Recompiled & Packaged** (Kernel & IRQ inited, native memory card icon extracted from disc) |
| **Castlevania: Symphony of the Night** | `SLUS-00067` | ✅ Clean (518 funcs) | 🟡 Boot & Overlay Init | 🟡 In progress | **Boots & Loads Subfolder Overlays** (Loads `DRA.BIN`, `F_MAP.BIN`, and `F_TITLE0.BIN` via recursive ISO search) |

---

## Technical Roadmap

### Implemented & Validated
- [x] Full MIPS-I R3000A instruction decoding & basic block reconstruction
- [x] Function discovery, delay slot folding, and CFG analysis
- [x] Psy-Q BIOS stub detection (`JR $t2`) as function terminators in CFG analysis
- [x] Function size safety capping (2048 insns) preventing Apple Clang parser stack overflow
- [x] Out-of-function branch-and-link preservation (`BGEZAL`, `BLTZAL`)
- [x] Kernel BIOS low RAM setup (`0x0000..0x2000`) with vector trampolines (`0x80`, `0xA0`, `0xB0`, `0xC0`), B0 & C0 tables
- [x] Psy-Q `GetC0Table` (`B0:56`) and `GetB0Table` (`B0:57`) vector pointers preventing memory corruption in `libcd`/`libsnd`
- [x] Accurate `HookEntryInt` (`B0:19`) & `ReturnFromException` (`B0:17`) clean stack unwinding preventing recursive Psy-Q ISR initialization
- [x] Host-accurate `longjmp` (`A0:14`) PC restoration in AOT recompiled MIPS code
- [x] Inverted Active High bitfield for `OutdatedPadGetButtons` (`B0:16`) preventing premature button press spam on boot
- [x] Extended BIOS HLE library routines (`memchr`, `rand`, `srand`, `malloc`, `free`, `calloc`, `realloc`, `firstfile`)
- [x] Unmapped PC interpreter fallback with direct BIOS vector dispatch (`0xA0`, `0xB0`, `0xC0`)
- [x] Full GP0 `StoreImage` (`0xC0..0xDF`, VRAM-to-CPU) image transfer execution
- [x] Synchronized `GPUSTAT` drawing mode bits 0..10 upon `GP0(0xE1)` and textured polygon draw commands
- [x] GP1 `Get GPU Info` (`0x10`) query support and latch in `GPUREAD` (`0x1F801810`)
- [x] Hardware-accurate DMA2 Linked List termination condition (Bit 23 `0x00800000` & zero header check)
- [x] Immediate block mode (`sync_mode == 0`) vs slice mode (`sync_mode == 1`) DMA word count differentiation
- [x] 60-frame heartbeat diagnostic telemetry in VBlank loop
- [x] PS1 standard 12-word `setjmp` (`A0:13`) & `longjmp` (`A0:14`) context layout
- [x] Basic MDEC MMIO (`0x1F801820` / `0x1F801824`) and DMA channel support preventing hang on FMV logos
- [x] Full GTE vector coprocessor instruction pipeline with unit test suite
- [x] Metal hardware-accelerated rasterization & 1024×512 VRAM pipeline
- [x] Hardware-accurate PS1 15-bit color semi-transparency rule (`0x8000` = opaque black, fixing transparent holes in menus)
- [x] Texture modulation saturation safeguards (`(r * tr) >> 7`) fixing black drop shadow corruptions
- [x] Sprite clipping (`clip_x2 + 1`, `clip_y2 + 1`) and UV coordinate wrapping
- [x] VRAM-to-VRAM copy coordinate masking (`GP0(0x80..0x9F)`)
- [x] Dual-format disc sector parser (2048-byte ISO & 2352-byte RAW Mode 2 Form 1)
- [x] Recursive ISO9660 subdirectory lookup (up to depth 4) with path prefix stripping (`BIN/`, `BOSS/`, `ST/`)
- [x] CD-ROM BFRD request register & in-flight buffer protection preventing `.NSF` streaming chunk loss
- [x] Non-destructive CD-ROM BFRD toggle preserving `data_bytes_avail` during DMA3 transfers
- [x] Accurate `GetlocL` / `GetlocP` sub-channel positioning via `last_read_lba`
- [x] Automated game icon pipeline (Disc TIM extraction + Libretro HD Boxart resolver)
- [x] Isolated build directories per game (`build/recompiled_<slug>/`) in pipeline
- [x] Zero-configuration pipeline with automatic title & BIOS detection
- [x] macOS `.app` bundle builder & compressed UDZO `.dmg` packaging

### Active Research & Debugging
- [x] Castlevania FMV streaming (byte-wide DICR/DMA register accesses)
- [x] SPU ADSR / interpolation / noise / pitch modulation; XA-ADPCM decoding
- [x] Memory card over SIO0 (`0x81`: read / write / get-ID), flushed to `~/Library/Application Support/MimiPS1/memcards/`
- [x] Metal presenter: 24-bit display mode (FMV)
- [x] Recompiler: delay-slot labels shared with jump targets, hand-built `$ra`, per-instruction cycle accounting
- [x] SPU reverb
- [x] CPU timing model: 1 cycle/instruction plus a stall for every load from RAM (the R3000A has no data cache); no per-title hacks
- [ ] SPU IRQ
- [ ] Software rasterizer speed / mask bit / dithering / texture-window accuracy
- [ ] Analog joystick mapping and rumble haptics via GameController framework

### Debugging Aids
| Variable | Effect |
| :--- | :--- |
| `MIMIPS1_LOG=1..3` | Log level (exceptions, CD/DMA/SIO traces) |
| `MIMIPS1_UNTHROTTLED=1` | Skip real-time frame pacing |
| `MIMIPS1_HLE=1` | Force the built-in HLE kernel instead of the real BIOS |
| `MIMIPS1_CD_SPEEDUP=N` | Shorten CD seek/transfer times |
| `MIMIPS1_INPUT="start@300+8"` | Scripted pad input (button@frame+duration) |
| `MIMIPS1_DUMP_FRAMES / _EVERY / _FROM` | Dump VRAM to `/tmp/vram_frame_N.ppm` (`tools/vram2png.py` crops/converts) |
| `MIMIPS1_AUDIO_STATS=1` | Once a second: output peak, active voices, XA frames queued |
| `MIMIPS1_FRAMESTATS=1` | Once a second: frames that missed the 60 Hz deadline, display buffer swaps, unacknowledged vblanks |
| `MIMIPS1_PADLOG=1` | One line per controller poll (video frame, port, button bits) |
| `MIMIPS1_MINPRESS=<n>` | Minimum frames a button stays down (default 8; short taps are stretched) |
| `MIMIPS1_MINGAP=<n>` | Frames a button stays up between two taps (default 6; a fast second tap is queued) |
| `MIMIPS1_RXLOG=1` | Log every SIO0 transmit, receive and status read (controller / memory card protocol) |
| `MIMIPS1_ACKCYC=<n>` | Cycles the /ACK line stays asserted after a byte (default 300) |
| `MIMIPS1_PRESENT_DUMP=<n>` / `MIMIPS1_PRESENT_EVERY=<k>` | Write the frame the Metal presenter actually shows to `/tmp/present*.ppm` |
| `MIMIPS1_CPI=<n>` | Base cycles per instruction (default 1.0, may be fractional; games time out polling loops by iteration count) |
| `MIMIPS1_LOADCOST=<n>` | Extra cycles for a CPU load from RAM (default 4; scratchpad loads are free). Too small and libpsx `VSync` loops time out |
| `MIMIPS1_TIMERLOG=1` / `MIMIPS1_MMIOHIST=1` | Root counter writes / per-second MMIO read histogram |
| `MIMIPS1_NONATIVE=1` | Run everything in the interpreter (reference behaviour for comparing with recompiled code) |
| `MIMIPS1_NATIVE_RANGE=lo-hi` | Hex address range of functions allowed to run natively (bisect native-code bugs) |
| `PS1_VERIFY=1 tools/fast_rebuild.sh ...` + `MIMIPS1_VERIFY=<frame>`, `_EVERY`, `_FN`, `MIMIPS1_STEP_FN` | Differential checker: sampled native calls are re-run in the interpreter and compared; `STEP_FN` writes per-instruction traces to `/tmp/step_*.txt` |
| `MIMIPS1_RAMDUMP=frame` | Dump RAM to `/tmp/ps1_ram.bin` (`tools/psdis.py` disassembles it) |
| `MIMIPS1_WATCH=addr`, `MIMIPS1_PCTRACE=1` | RAM word watchpoint; PC ring buffer on unexpected exceptions |
| `MIMIPS1_MDEC_LOG`, `MIMIPS1_MDEC_DUMP=N` | MDEC command log / decoded frame dumps |

`tools/cfg_coverage.py` reports uncovered text for a recompiled EXE; `tools/fast_rebuild.sh` rebuilds a game bundle's runtime only.

---

## License

This project is licensed under the [MIT License](LICENSE).

---

<p align="center">
  <sub>Built for macOS and Apple Silicon. PlayStation is a registered trademark of Sony Interactive Entertainment Inc. This project is an independent research endeavor and is not affiliated with or endorsed by Sony.</sub>
</p>
