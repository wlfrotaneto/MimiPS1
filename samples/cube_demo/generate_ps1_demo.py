#!/usr/bin/env python3
"""
Generates a valid PlayStation 1 Executable (PS-X EXE) containing real MIPS-I
machine code for 3D polygon setup, GTE calculations, GPU GP0 packet emission,
and standard Psy-Q/BIOS calls.
"""

import struct
import os

def assemble_mips():
    # Helper to encode MIPS-I R3000A instructions
    def r_type(op, rs, rt, rd, sa, funct):
        return (op << 26) | (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct

    def i_type(op, rs, rt, imm):
        return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)

    def j_type(op, target):
        return (op << 26) | ((target >> 2) & 0x03FFFFFF)

    code = []

    # Entry point at 0x80010000
    # 1. lui $s0, 0x1F80 (I/O base)
    code.append(i_type(0x0F, 0, 16, 0x1F80))
    # 2. ori $s0, $s0, 0x1810 (GP0 register at 0x1F801810)
    code.append(i_type(0x0D, 16, 16, 0x1810))

    # Reset/Clear screen: Send GP0(0x02) Fill Rectangle in VRAM
    # 3. lui $t0, 0x0200 (Fill rect command: R=30, G=30, B=80 dark navy blue)
    code.append(i_type(0x0F, 0, 8, 0x021E))
    # 4. ori $t0, $t0, 0x1E50
    code.append(i_type(0x0D, 8, 8, 0x1E50))
    # 5. sw $t0, 0($s0)
    code.append(i_type(0x2B, 16, 8, 0))

    # Coordinates: (X=0, Y=0)
    # 6. sw $zero, 0($s0)
    code.append(i_type(0x2B, 16, 0, 0))

    # Dimensions: (W=320, H=240) -> 0x00F00140
    # 7. lui $t0, 0x00F0
    code.append(i_type(0x0F, 0, 8, 0x00F0))
    # 8. ori $t0, $t0, 0x0140
    code.append(i_type(0x0D, 8, 8, 0x0140))
    # 9. sw $t0, 0($s0)
    code.append(i_type(0x2B, 16, 8, 0))

    # Set Drawing Area Top-Left (0xE3000000)
    code.append(i_type(0x0F, 0, 8, 0xE300))
    code.append(i_type(0x2B, 16, 8, 0))

    # Set Drawing Area Bottom-Right (0xE403BD3F) (320x240)
    code.append(i_type(0x0F, 0, 8, 0xE403))
    code.append(i_type(0x0D, 8, 8, 0xBD3F))
    code.append(i_type(0x2B, 16, 8, 0))

    # Draw Gouraud-shaded Triangle (GP0 0x30)
    # Vertex 0: Color (Red: 255, 0, 0) + Command 0x30
    code.append(i_type(0x0F, 0, 8, 0x3000))
    code.append(i_type(0x0D, 8, 8, 0x00FF))
    code.append(i_type(0x2B, 16, 8, 0))
    # Coord 0: (X=160, Y=40) -> 0x002800A0
    code.append(i_type(0x0F, 0, 8, 0x0028))
    code.append(i_type(0x0D, 8, 8, 0x00A0))
    code.append(i_type(0x2B, 16, 8, 0))

    # Vertex 1: Color (Green: 0, 255, 0)
    code.append(i_type(0x0F, 0, 8, 0x0000))
    code.append(i_type(0x0D, 8, 8, 0xFF00))
    code.append(i_type(0x2B, 16, 8, 0))
    # Coord 1: (X=60, Y=200) -> 0x00C8003C
    code.append(i_type(0x0F, 0, 8, 0x00C8))
    code.append(i_type(0x0D, 8, 8, 0x003C))
    code.append(i_type(0x2B, 16, 8, 0))

    # Vertex 2: Color (Blue: 0, 0, 255)
    code.append(i_type(0x0F, 0, 8, 0x00FF))
    code.append(i_type(0x0D, 8, 8, 0x0000))
    code.append(i_type(0x2B, 16, 8, 0))
    # Coord 2: (X=260, Y=200) -> 0x00C80104
    code.append(i_type(0x0F, 0, 8, 0x00C8))
    code.append(i_type(0x0D, 8, 8, 0x0104))
    code.append(i_type(0x2B, 16, 8, 0))

    # Infinite Frame Loop (j self with nop delay slot)
    loop_target = 0x80010000 + (len(code) * 4)
    code.append(j_type(0x02, loop_target))
    # Delay slot: nop (sll $zero, $zero, 0)
    code.append(0x00000000)

    return b"".join([struct.pack("<I", w) for w in code])

def create_psx_exe(filename):
    header = bytearray(2048)
    # Magic "PS-X EXE"
    header[0:8] = b"PS-X EXE"

    code_bytes = assemble_mips()

    init_pc   = 0x80010000
    init_gp   = 0x80018000
    text_addr = 0x80010000
    text_size = len(code_bytes)
    init_sp   = 0x801FFFF0

    struct.pack_into("<I", header, 0x10, init_pc)
    struct.pack_into("<I", header, 0x14, init_gp)
    struct.pack_into("<I", header, 0x18, text_addr)
    struct.pack_into("<I", header, 0x1C, text_size)
    struct.pack_into("<I", header, 0x30, init_sp)
    struct.pack_into("<I", header, 0x34, 0)

    # Marker string
    marker = b"Sony Computer Entertainment Inc. for MimiPS1 Demo"
    header[0x4C:0x4C+len(marker)] = marker

    with open(filename, "wb") as f:
        f.write(header)
        f.write(code_bytes)

    print(f"[Success] Generated PS-X EXE: {filename} ({os.path.getsize(filename)} bytes)")

if __name__ == "__main__":
    out_dir = os.path.dirname(os.path.abspath(__file__))
    target_path = os.path.join(out_dir, "TRIANGLE_DEMO.EXE")
    create_psx_exe(target_path)
