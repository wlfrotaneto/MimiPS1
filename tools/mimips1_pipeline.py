#!/usr/bin/env python3
"""
MimiPS1-Recomp End-to-End Pipeline
Automates:
 1. PlayStation 1 Binary (MIPS R3000A) Analysis & Disassembly
 2. High-Performance C Code Generation
 3. Native Apple Silicon (ARM64) Compilation via Clang
 4. macOS .app Bundle and .dmg Installer Packaging
"""

import os
import sys
import argparse
import subprocess
import shutil
import struct

ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BUILD_DIR = os.path.join(ROOT_DIR, "build")
DIST_DIR = os.path.join(ROOT_DIR, "dist")
RECOMPILER_BIN = os.path.join(BUILD_DIR, "mimips1-recomp")

# Terminal styling
CYAN   = "\033[1;36m"
GREEN  = "\033[1;32m"
YELLOW = "\033[1;33m"
BLUE   = "\033[1;34m"
BOLD   = "\033[1m"
RESET  = "\033[0m"

def print_banner():
    print(f"\n{CYAN}{BOLD}┌─────────────────────────────────────────────────────────────┐{RESET}")
    print(f"{CYAN}{BOLD}│  🎮 MimiPS1-Recomp - Ahead-of-Time Static Recompiler        │{RESET}")
    print(f"{CYAN}{BOLD}│  PlayStation 1 (MIPS) ➔ Native Apple Silicon macOS (.dmg)   │{RESET}")
    print(f"{CYAN}{BOLD}└─────────────────────────────────────────────────────────────┘{RESET}\n")

def run_command(cmd, cwd=ROOT_DIR, silent=False):
    res = subprocess.run(cmd, shell=True, cwd=cwd, capture_output=silent)
    if res.returncode != 0:
        if silent:
            print(res.stderr.decode("utf-8", errors="ignore"))
        print(f"\n❌ [Error] Command execution failed (exit code {res.returncode})")
        sys.exit(res.returncode)

def ensure_recompiler_built():
    if not os.path.isfile(RECOMPILER_BIN):
        print(f"{YELLOW}⚙️  Building mimips1-recomp tool...{RESET}")
        run_command(f"cmake -B {BUILD_DIR} -S {ROOT_DIR} && cmake --build {BUILD_DIR} --target mimips1-recomp", silent=True)

def extract_boot_executable(file_path, output_dir):
    """
    Inspects disc image (.bin, .cue, .iso).
    If detected, automatically extracts the boot executable referenced in SYSTEM.CNF.
    """
    if not os.path.isfile(file_path):
        return file_path

    ext = os.path.splitext(file_path)[1].lower()
    if ext == '.cue':
        cue_dir = os.path.dirname(file_path)
        with open(file_path, "r", encoding="utf-8", errors="ignore") as cf:
            for line in cf:
                if "FILE" in line.upper():
                    parts = line.split('"')
                    if len(parts) >= 2:
                        bin_candidate = os.path.join(cue_dir, parts[1])
                        if os.path.isfile(bin_candidate):
                            file_path = bin_candidate
                            break
                        # Also check if same dir has single .bin
                        for f in os.listdir(cue_dir):
                            if f.lower().endswith('.bin'):
                                file_path = os.path.join(cue_dir, f)
                                break
                    break
        ext = os.path.splitext(file_path)[1].lower()

    if ext not in ['.iso', '.bin', '.img']:
        return file_path

    print(f"💿 {BLUE}Inspecting CD-ROM disc image:{RESET} {os.path.basename(file_path)}")
    try:
        with open(file_path, "rb") as f:
            header = f.read(16)
            is_raw = header.startswith(b"\x00\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\x00")
            sector_size = 2352 if is_raw else 2048
            data_offset = 24 if is_raw else 0

            def read_sector(lba):
                f.seek(lba * sector_size + data_offset)
                return f.read(2048)

            pvd = read_sector(16)
            if b"CD001" not in pvd:
                if is_raw:
                    data_offset = 16
                    pvd = read_sector(16)
                if b"CD001" not in pvd:
                    return file_path

            root_rec = pvd[156:190]
            root_lba = struct.unpack("<I", root_rec[2:6])[0]
            root_size = struct.unpack("<I", root_rec[10:14])[0]

            root_sectors = (root_size + 2047) // 2048
            dir_data = b"".join(read_sector(root_lba + i) for i in range(root_sectors))[:root_size]

            files = {}
            pos = 0
            while pos < len(dir_data):
                length = dir_data[pos]
                if length == 0:
                    pos += 1
                    continue
                rec = dir_data[pos:pos+length]
                ext_lba = struct.unpack("<I", rec[2:6])[0]
                f_size = struct.unpack("<I", rec[10:14])[0]
                name_len = rec[32]
                name = rec[33:33+name_len].decode("ascii", errors="ignore").split(";")[0].upper()
                files[name] = (ext_lba, f_size)
                pos += length

            target_exe = None
            if "SYSTEM.CNF" in files:
                cnf_lba, cnf_size = files["SYSTEM.CNF"]
                cnf_sectors = (cnf_size + 2047) // 2048
                cnf_content = b"".join(read_sector(cnf_lba + i) for i in range(cnf_sectors))[:cnf_size].decode("ascii", errors="ignore")
                for line in cnf_content.splitlines():
                    if line.strip().upper().startswith("BOOT"):
                        parts = line.split("=")
                        if len(parts) > 1:
                            target_exe = parts[1].strip()
                            target_exe = target_exe.replace("cdrom:\\", "").replace("cdrom:/", "").replace("cdrom:", "").replace("CDROM:\\", "").replace("CDROM:/", "").replace("CDROM:", "").split(";")[0].strip().upper()
                            break

            if not target_exe:
                for fname in files:
                    if fname.endswith(".EXE") or fname.startswith("SLUS") or fname.startswith("SCES") or fname.startswith("SLES") or fname.startswith("SCUS"):
                        target_exe = fname
                        break

            if target_exe and target_exe in files:
                exe_lba, exe_size = files[target_exe]
                print(f"   🔍 Boot executable detected: {BOLD}{target_exe}{RESET} ({exe_size} bytes)")
                exe_sectors = (exe_size + 2047) // 2048
                exe_data = b"".join(read_sector(exe_lba + i) for i in range(exe_sectors))[:exe_size]

                out_exe = os.path.join(output_dir, target_exe)
                os.makedirs(output_dir, exist_ok=True)
                with open(out_exe, "wb") as out_f:
                    out_f.write(exe_data)
                print(f"   ✅ Boot executable extracted successfully: {out_exe}")
                return out_exe

    except Exception as e:
        print(f"   ⚠️  Automatic ISO extraction skipped ({e})")

    return file_path

def recompile_exe(exe_path, output_dir):
    ensure_recompiler_built()
    os.makedirs(output_dir, exist_ok=True)
    actual_exe = extract_boot_executable(exe_path, output_dir)
    print(f"⚙️  {BOLD}Analyzing and decompiling MIPS binary...{RESET}")
    run_command(f"{RECOMPILER_BIN} \"{actual_exe}\" -o \"{output_dir}\"")
    return actual_exe

def compile_native_app(recompiled_dir, app_name):
    print(f"⚡ {BOLD}Compiling native ARM64 binary with Apple Clang (-O3 -flto)...{RESET}")
    runtime_inc = os.path.join(ROOT_DIR, "runtime", "include")
    runtime_src_dir = os.path.join(ROOT_DIR, "runtime", "src")
    platform_dir = os.path.join(runtime_src_dir, "platform")
    
    runtime_sources = [
        os.path.join(runtime_src_dir, "mimips1_memory.c"),
        os.path.join(runtime_src_dir, "mimips1_cpu.c"),
        os.path.join(runtime_src_dir, "mimips1_gte.c"),
        os.path.join(runtime_src_dir, "mimips1_gpu.c"),
        os.path.join(runtime_src_dir, "mimips1_spu.c"),
        os.path.join(runtime_src_dir, "mimips1_dma.c"),
        os.path.join(runtime_src_dir, "mimips1_timers.c"),
        os.path.join(runtime_src_dir, "mimips1_cdrom.c"),
        os.path.join(runtime_src_dir, "mimips1_bios.c"),
        os.path.join(runtime_src_dir, "mimips1_input.c"),
        os.path.join(runtime_src_dir, "mimips1_card.c"),
        os.path.join(runtime_src_dir, "mimips1_runtime.c"),
        os.path.join(platform_dir, "macos_metal.m"),
        os.path.join(platform_dir, "macos_audio.m"),
        os.path.join(platform_dir, "macos_main.m"),
        os.path.join(runtime_src_dir, "recompiled_main.m"),
    ]
    
    recompiled_c = os.path.join(recompiled_dir, "recompiled_game.c")
    if os.path.isfile(recompiled_c):
        runtime_sources.append(recompiled_c)

    output_bin = os.path.join(BUILD_DIR, app_name)
    os.makedirs(BUILD_DIR, exist_ok=True)

    sources_str = " ".join([f'"{s}"' for s in runtime_sources])
    frameworks = "-framework Cocoa -framework Metal -framework MetalKit -framework AudioToolbox -framework GameController -framework CoreHaptics"
    cflags = f"-O2 -arch arm64 -fobjc-arc -I\"{runtime_inc}\" -I\"{recompiled_dir}\" -Wall -Wno-unused-variable -Wno-unused-label -Wno-parentheses"

    cmd = f"clang {cflags} {sources_str} {frameworks} -o \"{output_bin}\""
    run_command(cmd)
    return output_bin

def package_bundle_and_dmg(bin_path, app_name, disc_path=None, boot_exe=None, bios_path=None):
    # 1. Create .app
    print(f"📦 {BOLD}Building macOS app bundle: {app_name}.app...{RESET}")
    app_script = os.path.join(ROOT_DIR, "tools", "build_app_bundle.sh")
    disc_arg = f'"{disc_path}"' if disc_path and os.path.isfile(disc_path) else '""'
    boot_arg = f'"{boot_exe}"' if boot_exe and os.path.isfile(boot_exe) else '""'
    bios_arg = f'"{bios_path}"' if bios_path and os.path.isfile(bios_path) else '""'
    run_command(f"chmod +x \"{app_script}\" && \"{app_script}\" \"{bin_path}\" \"{app_name}\" \"{BUILD_DIR}\" {disc_arg} {boot_arg} {bios_arg}", silent=True)

    # 2. Create .dmg
    print(f"💿 {BOLD}Packaging DMG installer with UDZO compression...{RESET}")
    dmg_script = os.path.join(ROOT_DIR, "tools", "package_dmg.sh")
    app_path = os.path.join(BUILD_DIR, f"{app_name}.app")
    run_command(f"chmod +x \"{dmg_script}\" && \"{dmg_script}\" \"{app_path}\" \"{app_name}-AppleSilicon\" \"{DIST_DIR}\"", silent=True)

def resolve_input_path(input_path):
    if not os.path.isfile(input_path):
        return input_path
    ext = os.path.splitext(input_path)[1].lower()
    if ext == '.cue':
        cue_dir = os.path.dirname(os.path.abspath(input_path))
        try:
            with open(input_path, 'r', errors='ignore') as f:
                for line in f:
                    line = line.strip()
                    if line.upper().startswith("FILE"):
                        parts = line.split('"')
                        if len(parts) >= 2:
                            bin_name = parts[1]
                            candidate = os.path.join(cue_dir, bin_name)
                            if os.path.isfile(candidate):
                                print(f"🎵 {CYAN}Parsed CUE sheet:{RESET} referencing '{bin_name}'")
                                return candidate
        except Exception:
            pass
    return input_path

def main():
    print_banner()

    parser = argparse.ArgumentParser(description="MimiPS1 Recompilation Pipeline for macOS (Apple Silicon)")
    subparsers = parser.add_subparsers(dest="command")

    recomp_p = subparsers.add_parser("pipeline", help="Run full pipeline: Recompilation + Native Clang Build + DMG Packaging")
    recomp_p.add_argument("exe", help="Path to executable or disc image (.EXE, .BIN, .ISO, .CUE)")
    recomp_p.add_argument("--name", default="PS1RecompiledGame", help="macOS application name")
    recomp_p.add_argument("--output", default=os.path.join(BUILD_DIR, "recompiled"), help="Output directory for generated C source")
    recomp_p.add_argument("--embed-disc", action="store_true", default=True, help="Embed CD disc image into final app bundle (default: True)")
    recomp_p.add_argument("--no-embed-disc", dest="embed_disc", action="store_false", help="Do not embed CD disc image into app bundle")
    recomp_p.add_argument("--bios", default=None, help="Path to official PS1 BIOS image (e.g. SCPH1001.BIN)")
    recomp_p.add_argument("--install", "-i", action="store_true", help="Install generated .app directly into /Applications")
    recomp_p.add_argument("--no-dmg", action="store_true", help="Skip DMG disk image packaging")

    args = parser.parse_args()
    if args.command == "pipeline":
        actual_input = resolve_input_path(args.exe)
        print(f"▶️  {BOLD}[Step 1/3] Processing and Recompiling MIPS R3000A:{RESET} {actual_input}")
        boot_exe = recompile_exe(actual_input, args.output)

        print(f"\n▶️  {BOLD}[Step 2/3] Building Native Binary:{RESET} {args.name}")
        bin_path = compile_native_app(args.output, args.name)

        is_disc = os.path.isfile(actual_input) and os.path.splitext(actual_input)[1].lower() in ['.bin', '.iso', '.img']
        disc_to_embed = actual_input if (is_disc and args.embed_disc) else None

        bios_path = args.bios
        if not bios_path:
            candidate = os.path.expanduser("~/Downloads/SCPH1001.BIN")
            if os.path.isfile(candidate):
                bios_path = candidate
                print(f"🔍 {CYAN}Auto-detected user BIOS in Downloads:{RESET} {bios_path}")

        print(f"\n▶️  {BOLD}[Step 3/3] Packaging App Bundle and DMG Installer...{RESET}")
        app_path = os.path.join(BUILD_DIR, f"{args.name}.app")
        app_script = os.path.join(ROOT_DIR, "tools", "build_app_bundle.sh")
        disc_arg = f'"{disc_to_embed}"' if disc_to_embed else '""'
        boot_arg = f'"{boot_exe}"' if boot_exe and os.path.isfile(boot_exe) else '""'
        bios_arg = f'"{bios_path}"' if bios_path and os.path.isfile(bios_path) else '""'
        print(f"📦 {BOLD}Building macOS app bundle: {args.name}.app...{RESET}")
        run_command(f"chmod +x \"{app_script}\" && \"{app_script}\" \"{bin_path}\" \"{args.name}\" \"{BUILD_DIR}\" {disc_arg} {boot_arg} {bios_arg}", silent=True)

        dmg_file = None
        if not args.no_dmg:
            print(f"💿 {BOLD}Packaging DMG installer with UDZO compression...{RESET}")
            dmg_script = os.path.join(ROOT_DIR, "tools", "package_dmg.sh")
            run_command(f"chmod +x \"{dmg_script}\" && \"{dmg_script}\" \"{app_path}\" \"{args.name}-AppleSilicon\" \"{DIST_DIR}\"", silent=True)
            dmg_file = os.path.join(DIST_DIR, f"{args.name}-AppleSilicon.dmg")

        if args.install:
            dest_app = f"/Applications/{args.name}.app"
            print(f"🚀 {BOLD}Installing to /Applications:{RESET} {dest_app}...")
            run_command(f'rm -rf "{dest_app}" && cp -R "{app_path}" "/Applications/"')
            print(f"   {GREEN}✅ Successfully installed {args.name}.app to /Applications!{RESET}")

        print(f"\n{GREEN}{BOLD}═══════════════════════════════════════════════════════════════{RESET}")
        print(f"{GREEN}{BOLD}✨ Pipeline Completed Successfully!{RESET}")
        print(f"📦 {BOLD}Native App:{RESET}      {app_path}")
        if dmg_file:
            print(f"💿 {BOLD}DMG Installer:{RESET}   {dmg_file}")
        if args.install:
            print(f"🚀 {BOLD}Applications:{RESET}    /Applications/{args.name}.app")
        print(f"{GREEN}{BOLD}═══════════════════════════════════════════════════════════════{RESET}\n")
    else:
        parser.print_help()

if __name__ == "__main__":
    main()
