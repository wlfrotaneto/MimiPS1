# MimiPS1-Recomp Architecture & Technical Design

## 1. Overview
The MimiPS1 system is divided into two primary halves:
1. **Ahead-of-Time Static Binary Recompiler (`recompiler/`)**:
   - Parses PlayStation executables (PS-X EXE, ISO 9660, BIN/CUE).
   - Recovers Control Flow Graphs (CFG).
   - Detects delay slots, branch folds, call targets, and function entry points.
   - Emits structured, portable C code.
2. **Native Apple Silicon Runtime Engine (`runtime/`)**:
   - Implements the complete PlayStation 1 hardware memory bus, DMA, GTE, GPU, SPU, CD-ROM, and BIOS HLE.
   - Binds directly to native macOS Apple Silicon frameworks (Metal, CoreAudio, Cocoa, GameController).

## 2. Instruction Decoding & CFG Recovery
The MIPS R3000A processor features a 5-stage pipeline with branch delay slots and load delay slots.

### Branch Delay Slots in AOT C Code
When a branch instruction is encountered:
```mips
beq $a0, $a1, target
addiu $v0, $v0, 1
```
The transpiler emits:
```c
bool _take_branch = (R(4) == R(5));
loc_80010004:
R(2) = R(2) + 1; // Delay slot executed before branch resolution
if (_take_branch) goto loc_target;
```
This guarantees 100% architectural correctness with native C execution speed.

### Indirect Branch Dispatching
For jump tables and dynamic indirect function pointers (`jr $reg`):
The runtime allocates an $O(1)$ direct function table:
```c
#define PS1_FUNC_TABLE_SIZE (2 * 1024 * 1024 / 4) // 512,000 entries
PS1RecompiledFunc func_table[PS1_FUNC_TABLE_SIZE];
```
Any target address is dispatched with a single pointer indirection:
```c
func_table[(target_pc & 0x001FFFFC) >> 2](ctx);
```

## 3. Graphics & Metal Presentation
- **1024x512 VRAM**: Emulated in 16-bit BGR555 format.
- **Hardware Command FIFO**: GP0 packets render directly to VRAM in parallel.
- **Metal Pipeline**:
  - The VRAM framebuffer is mapped to an `MTLTexture`.
  - An MSL (Metal Shading Language) fragment shader performs scaling with optional CRT scanline filters and aspect ratio correction (4:3 original or widescreen).
  - Synchronized to the display refresh rate (60Hz / 120Hz ProMotion).

## 4. Audio Architecture
- **24-Voice ADPCM Synthesis**: SPU hardware decodes 16-byte blocks using 5-point prediction filters.
- **CoreAudio AudioQueue**: Zero-latency, thread-safe audio streaming in 44.1 kHz stereo PCM.
