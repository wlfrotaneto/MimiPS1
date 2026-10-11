# Complete PS1 Game Recompilation Guide on macOS

This guide provides step-by-step instructions for extracting the main boot binary from a PlayStation 1 disc image (`.iso` or `.bin/.cue`) and recompiling it into a native macOS `.dmg` installer for Apple Silicon (ARM64).

---

## 1. Locating the Main Boot Executable

On PlayStation 1 discs, the primary executable is specified in the `SYSTEM.CNF` configuration file located at the disc root.

Example `SYSTEM.CNF` contents:
```text
BOOT = cdrom:\SLUS_000.67;1
TCB = 4
EVENT = 16
STACK = 801FFFF0
```

In this example, `SLUS_000.67` (or similar naming schemes such as `SCES_xxx.xx`, `SLES_xxx.xx`, `SCUS_xxx.xx`, `PSX.EXE`) is the MIPS R3000A executable (`PS-X EXE`).

> **Note:** The MimiPS1 automated pipeline (`tools/mimips1_pipeline.py`) automatically parses the disc image, reads `SYSTEM.CNF`, and extracts the boot executable without requiring manual extraction.

If manual extraction is desired:
```bash
# Using 7z on an ISO image:
7z x game.iso SLUS_000.67
```

---

## 2. Recompiling with MimiPS1

Once you have the executable or disc image ready:

### Automated Pipeline (Single Command):

```bash
# Recompiling from a disc image or extracted executable:
python3 tools/mimips1_pipeline.py pipeline path/to/game.bin --name "GameTitle"
```

To create a standalone distribution bundle with the disc image embedded into `Contents/Resources/`:
```bash
python3 tools/mimips1_pipeline.py pipeline path/to/game.bin --name "GameTitle" --embed-disc
```

This single command executes the entire pipeline:
1. Disassembles and transpiles MIPS R3000A machine code to structured C.
2. Compiles a native ARM64 Mach-O binary using Apple Clang (`-O3 -flto`).
3. Packages the native macOS `GameTitle.app` bundle.
4. Generates a compressed installer at `dist/GameTitle-AppleSilicon.dmg`.

---

## 3. Installing and Running on macOS

1. Double-click the generated `.dmg` file in the `dist/` directory.
2. Drag the game application icon into the **Applications** folder shortcut.
3. Connect a gamepad (DualSense PS5, DualShock 4, Xbox Wireless, or Nintendo Switch Pro) via Bluetooth or USB.
4. Launch the application via Spotlight or Launchpad. The title executes natively with Metal hardware acceleration and low-latency CoreAudio.
