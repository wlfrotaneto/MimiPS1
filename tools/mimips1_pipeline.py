#!/usr/bin/env python3
"""
MimiPS1-Recomp End-to-End Pipeline
Automates:
 1. PlayStation 1 Binary (MIPS R3000A) Analysis & Disassembly
 2. High-Performance C Code Generation
 3. Native Apple Silicon (M4 / ARM64) Compilation via Clang
 4. macOS .app Bundle and .dmg Installer Packaging
"""

import os
import sys
import argparse
import subprocess
import shutil

ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BUILD_DIR = os.path.join(ROOT_DIR, "build")
DIST_DIR = os.path.join(ROOT_DIR, "dist")
RECOMPILER_BIN = os.path.join(BUILD_DIR, "mimips1-recomp")

def run_command(cmd, cwd=ROOT_DIR):
    print(f"==> Running: {cmd}")
    res = subprocess.run(cmd, shell=True, cwd=cwd)
    if res.returncode != 0:
        print(f"[Error] Command failed with code {res.returncode}")
        sys.exit(res.returncode)

def ensure_recompiler_built():
    if not os.path.isfile(RECOMPILER_BIN):
        print("==> Building mimips1-recomp CLI tool first...")
        run_command(f"cmake -B {BUILD_DIR} -S {ROOT_DIR} && cmake --build {BUILD_DIR} --target mimips1-recomp")

import struct

def extract_boot_executable(file_path, output_dir):
    """
    Inspects a .bin, .cue, or .iso PS1 image.
    If it's a disc image, parses ISO9660, reads SYSTEM.CNF,
    extracts the main PS-X EXE to output_dir, and returns its path.
    Otherwise returns file_path as is.
    """
    if not os.path.isfile(file_path):
        return file_path

    ext = os.path.splitext(file_path)[1].lower()
    if ext not in ['.iso', '.bin', '.img']:
        return file_path

    print(f"==> Inspecting CD-ROM disc image: {file_path}")
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
                            target_exe = target_exe.replace("cdrom:\\", "").replace("cdrom:/", "").split(";")[0].strip().upper()
                            break

            if not target_exe:
                for fname in files:
                    if fname.endswith(".EXE") or fname.startswith("SLUS") or fname.startswith("SCES") or fname.startswith("SLES"):
                        target_exe = fname
                        break

            if target_exe and target_exe in files:
                exe_lba, exe_size = files[target_exe]
                print(f"    [Disc Extractor] Found boot executable in SYSTEM.CNF: {target_exe} ({exe_size} bytes)")
                exe_sectors = (exe_size + 2047) // 2048
                exe_data = b"".join(read_sector(exe_lba + i) for i in range(exe_sectors))[:exe_size]

                out_exe = os.path.join(output_dir, target_exe)
                os.makedirs(output_dir, exist_ok=True)
                with open(out_exe, "wb") as out_f:
                    out_f.write(exe_data)
                print(f"    [Disc Extractor] Successfully extracted {target_exe} to {out_exe}")
                return out_exe

    except Exception as e:
        print(f"    [Notice] Automatic disc extraction skipped ({e})")

    return file_path

def recompile_exe(exe_path, output_dir):
    ensure_recompiler_built()
    os.makedirs(output_dir, exist_ok=True)
    actual_exe = extract_boot_executable(exe_path, output_dir)
    print(f"==> Recompiling PS1 executable: {actual_exe}")
    run_command(f"{RECOMPILER_BIN} \"{actual_exe}\" -o \"{output_dir}\"")

def compile_native_app(recompiled_dir, app_name):
    print(f"==> Compiling native Apple Silicon ARM64 binary for {app_name}...")
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
    frameworks = "-framework Cocoa -framework Metal -framework MetalKit -framework AudioToolbox -framework GameController"
    cflags = f"-O3 -flto -arch arm64 -I\"{runtime_inc}\" -I\"{recompiled_dir}\" -Wall"

    cmd = f"clang {cflags} {sources_str} {frameworks} -o \"{output_bin}\""
    run_command(cmd)
    return output_bin

def package_bundle_and_dmg(bin_path, app_name):
    # 1. Create .app
    app_script = os.path.join(ROOT_DIR, "tools", "build_app_bundle.sh")
    run_command(f"chmod +x \"{app_script}\" && \"{app_script}\" \"{bin_path}\" \"{app_name}\" \"{BUILD_DIR}\"")

    # 2. Create .dmg
    dmg_script = os.path.join(ROOT_DIR, "tools", "package_dmg.sh")
    app_path = os.path.join(BUILD_DIR, f"{app_name}.app")
    run_command(f"chmod +x \"{dmg_script}\" && \"{dmg_script}\" \"{app_path}\" \"{app_name}-AppleSilicon\" \"{DIST_DIR}\"")

def main():
    parser = argparse.ArgumentParser(description="MimiPS1 Recompilation Pipeline for macOS (Apple Silicon)")
    subparsers = parser.add_subparsers(dest="command")

    recomp_p = subparsers.add_parser("pipeline", help="Run full pipeline: recompile + native compile + dmg")
    recomp_p.add_argument("exe", help="Path to PS1 .EXE or .BIN")
    recomp_p.add_argument("--name", default="PS1RecompiledGame", help="Name of macOS Application")
    recomp_p.add_argument("--output", default=os.path.join(BUILD_DIR, "recompiled"), help="Recompiled C code output dir")

    args = parser.parse_args()
    if args.command == "pipeline":
        print(f"\n>>> [1/4] Recompiling MIPS R3000A Binary: {args.exe}")
        recompile_exe(args.exe, args.output)

        print(f"\n>>> [2/4] Compiling Native Apple Silicon App: {args.name}")
        bin_path = compile_native_app(args.output, args.name)

        print(f"\n>>> [3/4] Packaging macOS .app Bundle and DMG Installer...")
        package_bundle_and_dmg(bin_path, args.name)

        print(f"\n>>> [4/4] Pipeline Complete! Enjoy your native PS1 game on macOS M4!\n")
    else:
        parser.print_help()

if __name__ == "__main__":
    main()
