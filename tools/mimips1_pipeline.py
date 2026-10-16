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
import urllib.request
import urllib.parse
import re

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
    recompiler_inc = os.path.join(ROOT_DIR, "recompiler", "include")
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
        os.path.join(ROOT_DIR, "recompiler", "src", "mips_disasm.c"),
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
    cflags = f"-O2 -arch arm64 -fobjc-arc -I\"{runtime_inc}\" -I\"{recompiler_inc}\" -I\"{recompiled_dir}\" -Wall -Wno-unused-variable -Wno-unused-label -Wno-parentheses"

    cmd = f"clang {cflags} {sources_str} {frameworks} -o \"{output_bin}\""
    run_command(cmd)
    return output_bin

def decode_tim_to_png(tim_data, out_png_path, target_size=512):
    """Decodes PS1 TIM texture data into an uncompressed BMP and converts to PNG using macOS sips."""
    try:
        if len(tim_data) < 16:
            return False
        magic, flags = struct.unpack("<II", tim_data[:8])
        if magic != 0x00000010:
            return False
        pmode = flags & 7
        has_clut = (flags & 8) != 0

        offset = 8
        palette = []
        if has_clut:
            clut_len = struct.unpack("<I", tim_data[offset:offset+4])[0]
            clut_raw = tim_data[offset+12:offset+clut_len]
            num_colors = min(256, len(clut_raw) // 2)
            for i in range(num_colors):
                c = struct.unpack("<H", clut_raw[i*2:i*2+2])[0]
                r = int((c & 0x1F) * 255 / 31)
                g = int(((c >> 5) & 0x1F) * 255 / 31)
                b = int(((c >> 10) & 0x1F) * 255 / 31)
                a = 0 if (i == 0 or (c == 0)) else 255
                palette.append((r, g, b, a))
            offset += clut_len

        pix_len = struct.unpack("<I", tim_data[offset:offset+4])[0]
        pdx, pdy, pw, ph = struct.unpack("<HHHH", tim_data[offset+4:offset+12])
        pix_raw = tim_data[offset+12:offset+pix_len]

        if pmode == 0:
            w_pixels = pw * 4
        elif pmode == 1:
            w_pixels = pw * 2
        elif pmode == 2:
            w_pixels = pw
        else:
            return False

        h_pixels = ph
        # Take the first square frame if animated/tiled vertically
        if h_pixels > w_pixels and (h_pixels % w_pixels == 0):
            h_pixels = w_pixels

        if w_pixels == 0 or h_pixels == 0:
            return False

        grid = []
        for y in range(h_pixels):
            row = []
            if pmode == 0:
                row_bytes = (w_pixels + 1) // 2
                row_data = pix_raw[y * pw * 2 : y * pw * 2 + row_bytes]
                for byte in row_data:
                    p0 = byte & 0x0F
                    p1 = (byte >> 4) & 0x0F
                    row.append(palette[p0] if p0 < len(palette) else (0, 0, 0, 0))
                    if len(row) < w_pixels:
                        row.append(palette[p1] if p1 < len(palette) else (0, 0, 0, 0))
            elif pmode == 1:
                row_data = pix_raw[y * pw * 2 : y * pw * 2 + w_pixels]
                for idx in row_data:
                    row.append(palette[idx] if idx < len(palette) else (0, 0, 0, 0))
            elif pmode == 2:
                for x in range(w_pixels):
                    off = (y * pw + x) * 2
                    if off + 2 <= len(pix_raw):
                        c = struct.unpack("<H", pix_raw[off:off+2])[0]
                        r = int((c & 0x1F) * 255 / 31)
                        g = int(((c >> 5) & 0x1F) * 255 / 31)
                        b = int(((c >> 10) & 0x1F) * 255 / 31)
                        a = 0 if c == 0 else 255
                        row.append((r, g, b, a))
                    else:
                        row.append((0, 0, 0, 0))
            grid.append(row)

        scale = max(1, target_size // max(w_pixels, h_pixels))
        out_w = w_pixels * scale
        out_h = h_pixels * scale
        row_bytes = out_w * 4
        img_size = row_bytes * out_h

        bmp_path = out_png_path + ".bmp"
        with open(bmp_path, "wb") as f:
            f.write(b"BM")
            f.write(struct.pack("<IHHI", 54 + img_size, 0, 0, 54))
            f.write(struct.pack("<IIIHHIIIIII", 40, out_w, out_h, 1, 32, 0, img_size, 2835, 2835, 0, 0))
            for y in reversed(range(h_pixels)):
                for _ in range(scale):
                    for x in range(w_pixels):
                        r, g, b, a = grid[y][x]
                        for _ in range(scale):
                            f.write(bytes([b, g, r, a]))

        subprocess.run(["sips", "-s", "format", "png", bmp_path, "--out", out_png_path], capture_output=True, check=True)
        if os.path.exists(bmp_path):
            os.remove(bmp_path)
        return True
    except Exception:
        return False

def png_to_icns(png_path, out_icns_path):
    """Converts a PNG image to a macOS .icns file using native sips and iconutil."""
    try:
        iconset_dir = out_icns_path + ".iconset"
        os.makedirs(iconset_dir, exist_ok=True)
        sizes = [16, 32, 64, 128, 256, 512]
        for s in sizes:
            subprocess.run(["sips", "-z", str(s), str(s), png_path, "--out", os.path.join(iconset_dir, f"icon_{s}x{s}.png")], capture_output=True)
            if s * 2 <= 1024:
                subprocess.run(["sips", "-z", str(s*2), str(s*2), png_path, "--out", os.path.join(iconset_dir, f"icon_{s}x{s}@2x.png")], capture_output=True)
        subprocess.run(["iconutil", "-c", "icns", iconset_dir, "-o", out_icns_path], capture_output=True, check=True)
        shutil.rmtree(iconset_dir, ignore_errors=True)
        return True
    except Exception:
        return False

def find_tim_icon_in_disc(disc_path):
    """Recursively scans disc ISO9660 filesystem for ICON.TIM or *.TIM with ICON in name."""
    try:
        with open(disc_path, "rb") as f:
            header = f.read(16)
            is_raw = header.startswith(b"\x00\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\x00")
            sector_size = 2352 if is_raw else 2048
            data_offset = 24 if is_raw else 0

            def read_sector(lba):
                f.seek(lba * sector_size + data_offset)
                return f.read(2048)

            pvd = read_sector(16)
            if b"CD001" not in pvd and is_raw:
                data_offset = 16
                pvd = read_sector(16)
            if b"CD001" not in pvd:
                return None

            root_rec = pvd[156:190]
            root_lba = struct.unpack("<I", root_rec[2:6])[0]
            root_size = struct.unpack("<I", root_rec[10:14])[0]

            def scan_dir(lba, size, depth=0):
                if depth > 3: return None
                sectors = (size + 2047) // 2048
                data = b"".join(read_sector(lba + i) for i in range(sectors))[:size]
                pos = 0
                subdirs = []
                while pos < len(data):
                    length = data[pos]
                    if length == 0:
                        pos = ((pos // 2048) + 1) * 2048
                        if pos >= len(data): break
                        continue
                    rec = data[pos:pos+length]
                    flags = rec[25]
                    name_len = rec[32]
                    name = rec[33:33+name_len].decode("ascii", errors="ignore").split(";")[0].upper()
                    ext_lba = struct.unpack("<I", rec[2:6])[0]
                    f_size = struct.unpack("<I", rec[10:14])[0]
                    is_dir = (flags & 2) != 0

                    if not is_dir and name:
                        if "ICON" in name and name.endswith(".TIM"):
                            f.seek(ext_lba * sector_size + data_offset)
                            return f.read(f_size)
                    elif is_dir and name not in ("", "\x00", "\x01"):
                        subdirs.append((ext_lba, f_size))
                    pos += length

                for sub_lba, sub_size in subdirs:
                    res = scan_dir(sub_lba, sub_size, depth + 1)
                    if res: return res
                return None

            return scan_dir(root_lba, root_size)
    except Exception:
        return None

def fetch_boxart_png(game_title, out_png_path):
    """Attempts to download official high-res Boxart from Libretro thumbnails repository."""
    base_url = "https://raw.githubusercontent.com/libretro-thumbnails/Sony_-_PlayStation/master/Named_Boxarts/"
    variants = [
        f"{game_title} (USA).png",
        f"{game_title} (Europe).png",
        f"{game_title} (Japan).png",
        f"{game_title}.png",
    ]
    for v in variants:
        encoded = urllib.parse.quote(v)
        url = base_url + encoded
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "MimiPS1/1.0"})
            with urllib.request.urlopen(req, timeout=4) as resp:
                if resp.status == 200:
                    with open(out_png_path, "wb") as out_f:
                        out_f.write(resp.read())
                    return True
        except Exception:
            pass
    return False

def resolve_game_icon(app_name, disc_path=None, custom_icon=None):
    """
    Resolves the best icon for the game app bundle:
    1. Custom icon from CLI argument (--icon)
    2. Cached icon in assets/icons/{app_name}.icns or assets/{app_name}.icns
    3. Extracted PS1 Memory Card / TIM icon directly from disc
    4. Auto-fetched official high-res Boxart from public repository
    5. Fallback to generic PlayStation controller icon
    """
    icons_cache_dir = os.path.join(ROOT_DIR, "assets", "icons")
    os.makedirs(icons_cache_dir, exist_ok=True)
    temp_dir = os.path.join(BUILD_DIR, "icon_temp")
    os.makedirs(temp_dir, exist_ok=True)

    # 1. Custom icon from argument
    if custom_icon and os.path.isfile(custom_icon):
        if custom_icon.endswith(".icns"):
            return custom_icon
        out_icns = os.path.join(icons_cache_dir, f"{app_name}.icns")
        if png_to_icns(custom_icon, out_icns):
            return out_icns

    # 2. Existing cached icon
    candidates = [
        os.path.join(icons_cache_dir, f"{app_name}.icns"),
        os.path.join(ROOT_DIR, "assets", f"{app_name}.icns"),
    ]
    for c in candidates:
        if os.path.isfile(c):
            print(f"🎨 {CYAN}Using game icon:{RESET} {os.path.basename(c)}")
            return c

    # 3. Try to extract icon from disc image
    if disc_path and os.path.isfile(disc_path):
        tim_data = find_tim_icon_in_disc(disc_path)
        if tim_data:
            tim_png = os.path.join(temp_dir, "disc_icon.png")
            out_icns = os.path.join(icons_cache_dir, f"{app_name}.icns")
            if decode_tim_to_png(tim_data, tim_png, target_size=512) and png_to_icns(tim_png, out_icns):
                print(f"🕹️  {GREEN}Successfully extracted PS1 Memory Card icon from disc:{RESET} {app_name}.icns")
                return out_icns

    # 4. Try auto-fetching official high-res Boxart
    search_titles = []
    if disc_path:
        base = os.path.splitext(os.path.basename(disc_path))[0]
        clean_base = re.sub(r"\[.*?\]", "", base)
        clean_base = re.sub(r"\(.*?\)", "", clean_base).strip()
        if clean_base:
            search_titles.append(clean_base)

    split_name = re.sub(r"([a-z])([A-Z])", r"\1 \2", app_name)
    if split_name not in search_titles:
        search_titles.append(split_name)
    if app_name not in search_titles:
        search_titles.append(app_name)

    for st in search_titles:
        boxart_png = os.path.join(temp_dir, "boxart.png")
        if fetch_boxart_png(st, boxart_png):
            out_icns = os.path.join(icons_cache_dir, f"{app_name}.icns")
            if png_to_icns(boxart_png, out_icns):
                print(f"🌐 {GREEN}Auto-fetched official high-res Boxart icon:{RESET} {st} -> {app_name}.icns")
                return out_icns

    # 5. Fallback
    fallback = os.path.join(ROOT_DIR, "assets", "AppIcon.icns")
    if os.path.isfile(fallback):
        return fallback
    return None

def package_bundle_and_dmg(bin_path, app_name, disc_path=None, boot_exe=None, bios_path=None, icon_path=None):
    # 1. Create .app
    print(f"📦 {BOLD}Building macOS app bundle: {app_name}.app...{RESET}")
    app_script = os.path.join(ROOT_DIR, "tools", "build_app_bundle.sh")
    disc_arg = f'"{disc_path}"' if disc_path and os.path.isfile(disc_path) else '""'
    boot_arg = f'"{boot_exe}"' if boot_exe and os.path.isfile(boot_exe) else '""'
    bios_arg = f'"{bios_path}"' if bios_path and os.path.isfile(bios_path) else '""'
    icon_arg = f'"{icon_path}"' if icon_path and os.path.isfile(icon_path) else '""'
    run_command(f"chmod +x \"{app_script}\" && \"{app_script}\" \"{bin_path}\" \"{app_name}\" \"{BUILD_DIR}\" {disc_arg} {boot_arg} {bios_arg} {icon_arg}", silent=True)

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

def auto_detect_game_name(input_path, boot_exe=None):
    """Automatically extracts a clean, human-readable game name from file name or disc serial."""
    base = os.path.splitext(os.path.basename(input_path))[0]
    for ext in ['.bin', '.iso', '.img', '.cue', '.exe']:
        if base.lower().endswith(ext):
            base = base[:-len(ext)]

    clean = re.sub(r"\[.*?\]", "", base)
    clean = re.sub(r"\(.*?\)", "", clean)
    clean = re.sub(r"\s+", " ", clean).strip()

    upper = clean.upper()
    is_generic = not clean or any(upper.startswith(p) for p in ["TRACK", "DISC", "CDROM", "IMAGE", "GAME"])
    if not is_generic:
        return clean

    if boot_exe:
        exe_name = os.path.basename(boot_exe).upper()
        serial = exe_name.replace("_", "-").split(";")[0].replace(".", "")
        if serial:
            return serial

    return "PS1Game"

def main():
    print_banner()

    parser = argparse.ArgumentParser(description="MimiPS1 Recompilation Pipeline for macOS (Apple Silicon)")
    subparsers = parser.add_subparsers(dest="command")

    recomp_p = subparsers.add_parser("pipeline", help="Run full pipeline: Recompilation + Native Clang Build + DMG Packaging")
    recomp_p.add_argument("exe", help="Path to executable or disc image (.EXE, .BIN, .ISO, .CUE)")
    recomp_p.add_argument("--name", default=None, help="macOS application name (auto-detected if omitted)")
    recomp_p.add_argument("--output", default=os.path.join(BUILD_DIR, "recompiled"), help="Output directory for generated C source")
    recomp_p.add_argument("--embed-disc", action="store_true", default=True, help="Embed CD disc image into final app bundle (default: True)")
    recomp_p.add_argument("--no-embed-disc", dest="embed_disc", action="store_false", help="Do not embed CD disc image into app bundle")
    recomp_p.add_argument("--bios", default=None, help="Path to official PS1 BIOS image (e.g. SCPH1001.BIN)")
    recomp_p.add_argument("--icon", default=None, help="Path to custom application icon (.icns or .png)")
    recomp_p.add_argument("--install", "-i", action="store_true", help="Install generated .app directly into /Applications")
    recomp_p.add_argument("--no-dmg", action="store_true", help="Skip DMG disk image packaging")

    args = parser.parse_args()
    if args.command == "pipeline":
        actual_input = resolve_input_path(args.exe)
        app_name = args.name
        if not app_name:
            app_name = auto_detect_game_name(actual_input)
            print(f"🎮 {CYAN}Auto-detected application name:{RESET} {BOLD}{app_name}{RESET}")

        recomp_dir = args.output
        if recomp_dir == os.path.join(BUILD_DIR, "recompiled"):
            slug = re.sub(r'[^a-zA-Z0-9_-]', '_', app_name).lower().strip('_')
            recomp_dir = os.path.join(BUILD_DIR, f"recompiled_{slug}")

        print(f"▶️  {BOLD}[Step 1/3] Processing and Recompiling MIPS R3000A:{RESET} {actual_input}")
        boot_exe = recompile_exe(actual_input, recomp_dir)

        print(f"\n▶️  {BOLD}[Step 2/3] Building Native Binary:{RESET} {app_name}")
        bin_path = compile_native_app(recomp_dir, app_name)

        is_disc = os.path.isfile(actual_input) and os.path.splitext(actual_input)[1].lower() in ['.bin', '.iso', '.img']
        disc_to_embed = actual_input if (is_disc and args.embed_disc) else None

        bios_path = args.bios
        if not bios_path:
            candidates = [
                os.path.join(ROOT_DIR, "assets", "bios.bin"),
                os.path.expanduser("~/Downloads/SCPH1001.BIN"),
                os.path.expanduser("~/Downloads/bios.bin"),
            ]
            for candidate in candidates:
                if os.path.isfile(candidate):
                    bios_path = candidate
                    print(f"🔍 {CYAN}Auto-detected PS1 BIOS:{RESET} {bios_path}")
                    break

        print(f"\n▶️  {BOLD}[Step 3/3] Packaging App Bundle and DMG Installer...{RESET}")
        icon_path = resolve_game_icon(app_name, disc_path=disc_to_embed, custom_icon=args.icon)
        app_path = os.path.join(BUILD_DIR, f"{app_name}.app")
        app_script = os.path.join(ROOT_DIR, "tools", "build_app_bundle.sh")
        disc_arg = f'"{disc_to_embed}"' if disc_to_embed else '""'
        boot_arg = f'"{boot_exe}"' if boot_exe and os.path.isfile(boot_exe) else '""'
        bios_arg = f'"{bios_path}"' if bios_path and os.path.isfile(bios_path) else '""'
        icon_arg = f'"{icon_path}"' if icon_path and os.path.isfile(icon_path) else '""'
        print(f"📦 {BOLD}Building macOS app bundle: {app_name}.app...{RESET}")
        run_command(f"chmod +x \"{app_script}\" && \"{app_script}\" \"{bin_path}\" \"{app_name}\" \"{BUILD_DIR}\" {disc_arg} {boot_arg} {bios_arg} {icon_arg}", silent=True)

        dmg_file = None
        if not args.no_dmg:
            print(f"💿 {BOLD}Packaging DMG installer with UDZO compression...{RESET}")
            dmg_script = os.path.join(ROOT_DIR, "tools", "package_dmg.sh")
            run_command(f"chmod +x \"{dmg_script}\" && \"{dmg_script}\" \"{app_path}\" \"{app_name}-AppleSilicon\" \"{DIST_DIR}\"", silent=True)
            dmg_file = os.path.join(DIST_DIR, f"{app_name}-AppleSilicon.dmg")

        if args.install:
            dest_app = f"/Applications/{app_name}.app"
            print(f"🚀 {BOLD}Installing to /Applications:{RESET} {dest_app}...")
            run_command(f'rm -rf "{dest_app}" && cp -R "{app_path}" "/Applications/"')
            print(f"   {GREEN}✅ Successfully installed {app_name}.app to /Applications!{RESET}")

        print(f"\n{GREEN}{BOLD}═══════════════════════════════════════════════════════════════{RESET}")
        print(f"{GREEN}{BOLD}✨ Pipeline Completed Successfully!{RESET}")
        print(f"📦 {BOLD}Native App:{RESET}      {app_path}")
        if dmg_file:
            print(f"💿 {BOLD}DMG Installer:{RESET}   {dmg_file}")
        if args.install:
            print(f"🚀 {BOLD}Applications:{RESET}    /Applications/{app_name}.app")
        print(f"{GREEN}{BOLD}═══════════════════════════════════════════════════════════════{RESET}\n")
    else:
        parser.print_help()

if __name__ == "__main__":
    main()
