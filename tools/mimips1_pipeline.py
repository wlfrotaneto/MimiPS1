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

def recompile_exe(exe_path, output_dir):
    ensure_recompiler_built()
    os.makedirs(output_dir, exist_ok=True)
    print(f"==> Recompiling PS1 executable: {exe_path}")
    run_command(f"{RECOMPILER_BIN} \"{exe_path}\" -o \"{output_dir}\"")

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
