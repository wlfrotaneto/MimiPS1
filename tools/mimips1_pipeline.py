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
        print(f"\n❌ [Erro] Falha ao executar comando (código {res.returncode})")
        sys.exit(res.returncode)

def ensure_recompiler_built():
    if not os.path.isfile(RECOMPILER_BIN):
        print(f"{YELLOW}⚙️  Compilando a ferramenta mimips1-recomp...{RESET}")
        run_command(f"cmake -B {BUILD_DIR} -S {ROOT_DIR} && cmake --build {BUILD_DIR} --target mimips1-recomp", silent=True)

def extract_boot_executable(file_path, output_dir):
    """
    Inspeciona se o arquivo é imagem de disco (.bin, .cue, .iso).
    Se for, extrai automaticamente o executável referenciado no SYSTEM.CNF.
    """
    if not os.path.isfile(file_path):
        return file_path

    ext = os.path.splitext(file_path)[1].lower()
    if ext not in ['.iso', '.bin', '.img']:
        return file_path

    print(f"💿 {BLUE}Inspecionando imagem de CD-ROM:{RESET} {os.path.basename(file_path)}")
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
                print(f"   🔍 Executável de boot detectado: {BOLD}{target_exe}{RESET} ({exe_size} bytes)")
                exe_sectors = (exe_size + 2047) // 2048
                exe_data = b"".join(read_sector(exe_lba + i) for i in range(exe_sectors))[:exe_size]

                out_exe = os.path.join(output_dir, target_exe)
                os.makedirs(output_dir, exist_ok=True)
                with open(out_exe, "wb") as out_f:
                    out_f.write(exe_data)
                print(f"   ✅ Executável extraído com sucesso para: {out_exe}")
                return out_exe

    except Exception as e:
        print(f"   ⚠️  Extração automática de ISO ignorada ({e})")

    return file_path

def recompile_exe(exe_path, output_dir):
    ensure_recompiler_built()
    os.makedirs(output_dir, exist_ok=True)
    actual_exe = extract_boot_executable(exe_path, output_dir)
    print(f"⚙️  {BOLD}Analisando e recompilando binário MIPS...{RESET}")
    run_command(f"{RECOMPILER_BIN} \"{actual_exe}\" -o \"{output_dir}\"")

def compile_native_app(recompiled_dir, app_name):
    print(f"⚡ {BOLD}Compilando código nativo ARM64 com Apple Clang (-O3 -flto)...{RESET}")
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
    cflags = f"-O3 -flto -arch arm64 -I\"{runtime_inc}\" -I\"{recompiled_dir}\" -Wall -Wno-unused-variable -Wno-unused-label -Wno-parentheses"

    cmd = f"clang {cflags} {sources_str} {frameworks} -o \"{output_bin}\""
    run_command(cmd)
    return output_bin

def package_bundle_and_dmg(bin_path, app_name, disc_path=None):
    # 1. Create .app
    print(f"📦 {BOLD}Construindo pacote macOS {app_name}.app...{RESET}")
    app_script = os.path.join(ROOT_DIR, "tools", "build_app_bundle.sh")
    disc_arg = f'"{disc_path}"' if disc_path and os.path.isfile(disc_path) else '""'
    run_command(f"chmod +x \"{app_script}\" && \"{app_script}\" \"{bin_path}\" \"{app_name}\" \"{BUILD_DIR}\" {disc_arg}", silent=True)

    # 2. Create .dmg
    print(f"💿 {BOLD}Empacotando instalador DMG com compressão UDZO...{RESET}")
    dmg_script = os.path.join(ROOT_DIR, "tools", "package_dmg.sh")
    app_path = os.path.join(BUILD_DIR, f"{app_name}.app")
    run_command(f"chmod +x \"{dmg_script}\" && \"{dmg_script}\" \"{app_path}\" \"{app_name}-AppleSilicon\" \"{DIST_DIR}\"", silent=True)

def main():
    print_banner()

    parser = argparse.ArgumentParser(description="MimiPS1 Recompilation Pipeline for macOS (Apple Silicon)")
    subparsers = parser.add_subparsers(dest="command")

    recomp_p = subparsers.add_parser("pipeline", help="Executa o pipeline completo: Recompilação + Compilação Nativa + DMG")
    recomp_p.add_argument("exe", help="Caminho do arquivo (.EXE, .BIN ou .ISO)")
    recomp_p.add_argument("--name", default="PS1RecompiledGame", help="Nome do aplicativo macOS")
    recomp_p.add_argument("--output", default=os.path.join(BUILD_DIR, "recompiled"), help="Pasta de saída do código C")
    recomp_p.add_argument("--embed-disc", action="store_true", help="Incorpora a imagem de CD (.bin/.iso) no bundle final")

    args = parser.parse_args()
    if args.command == "pipeline":
        print(f"▶️  {BOLD}[Passo 1/4] Processando e Recompilando MIPS R3000A:{RESET} {args.exe}")
        recompile_exe(args.exe, args.output)

        print(f"\n▶️  {BOLD}[Passo 2/4] Gerando Binário Nativo:{RESET} {args.name}")
        bin_path = compile_native_app(args.output, args.name)

        is_disc = os.path.isfile(args.exe) and os.path.splitext(args.exe)[1].lower() in ['.bin', '.iso', '.img']
        disc_to_embed = args.exe if (is_disc and args.embed_disc) else None

        print(f"\n▶️  {BOLD}[Passo 3/4] Empacotando Aplicativo e Instalador DMG...{RESET}")
        package_bundle_and_dmg(bin_path, args.name, disc_path=disc_to_embed)

        dmg_file = os.path.join(DIST_DIR, f"{args.name}-AppleSilicon.dmg")
        app_file = os.path.join(BUILD_DIR, f"{args.name}.app")

        print(f"\n{GREEN}{BOLD}═══════════════════════════════════════════════════════════════{RESET}")
        print(f"{GREEN}{BOLD}✨ Pipeline Concluído com Sucesso!{RESET}")
        print(f"📦 {BOLD}Aplicativo Nativo:{RESET} {app_file}")
        print(f"💿 {BOLD}Instalador DMG:{RESET}    {dmg_file}")
        print(f"{GREEN}{BOLD}═══════════════════════════════════════════════════════════════{RESET}\n")
    else:
        parser.print_help()

if __name__ == "__main__":
    main()
