#!/usr/bin/env python3
"""Tiny MIPS-I disassembler for debugging RAM dumps.

usage: psdis.py <ram.bin> <start_hex> <count> [base_hex=0]
"""
import struct
import sys

R = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
     's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']
SPECIAL = {0: 'sll', 2: 'srl', 3: 'sra', 4: 'sllv', 6: 'srlv', 7: 'srav', 8: 'jr', 9: 'jalr', 12: 'syscall', 13: 'break',
           16: 'mfhi', 17: 'mthi', 18: 'mflo', 19: 'mtlo', 24: 'mult', 25: 'multu', 26: 'div', 27: 'divu',
           32: 'add', 33: 'addu', 34: 'sub', 35: 'subu', 36: 'and', 37: 'or', 38: 'xor', 39: 'nor', 42: 'slt', 43: 'sltu'}
OPS = {2: 'j', 3: 'jal', 4: 'beq', 5: 'bne', 6: 'blez', 7: 'bgtz', 8: 'addi', 9: 'addiu', 10: 'slti', 11: 'sltiu',
       12: 'andi', 13: 'ori', 14: 'xori', 15: 'lui', 32: 'lb', 33: 'lh', 34: 'lwl', 35: 'lw', 36: 'lbu', 37: 'lhu',
       38: 'lwr', 40: 'sb', 41: 'sh', 42: 'swl', 43: 'sw', 46: 'swr', 50: 'lwc2', 58: 'swc2'}


def dis(x, a):
    if x == 0:
        return 'nop'
    op = x >> 26
    rs, rt, rd, sa = (x >> 21) & 31, (x >> 16) & 31, (x >> 11) & 31, (x >> 6) & 31
    imm = x & 0xFFFF
    simm = imm - 0x10000 if imm & 0x8000 else imm
    if op == 0:
        fn = x & 63
        n = SPECIAL.get(fn, 'special%02x' % fn)
        if fn in (0, 2, 3):
            return '%s %s,%s,%d' % (n, R[rd], R[rt], sa)
        if fn in (4, 6, 7):
            return '%s %s,%s,%s' % (n, R[rd], R[rt], R[rs])
        if fn == 8:
            return 'jr %s' % R[rs]
        if fn == 9:
            return 'jalr %s,%s' % (R[rd], R[rs])
        if fn in (12, 13):
            return n
        if fn in (16, 18):
            return '%s %s' % (n, R[rd])
        if fn in (17, 19):
            return '%s %s' % (n, R[rs])
        if fn in (24, 25, 26, 27):
            return '%s %s,%s' % (n, R[rs], R[rt])
        return '%s %s,%s,%s' % (n, R[rd], R[rs], R[rt])
    if op == 1:
        n = {0: 'bltz', 1: 'bgez', 16: 'bltzal', 17: 'bgezal'}.get(rt, 'regimm%d' % rt)
        return '%s %s,%08x' % (n, R[rs], (a + 4 + simm * 4) & 0xFFFFFFFF)
    if op in (2, 3):
        return '%s %08x' % (OPS[op], ((x & 0x3FFFFFF) << 2) | (a & 0xF0000000))
    if op in (4, 5):
        return '%s %s,%s,%08x' % (OPS[op], R[rs], R[rt], (a + 4 + simm * 4) & 0xFFFFFFFF)
    if op in (6, 7):
        return '%s %s,%08x' % (OPS[op], R[rs], (a + 4 + simm * 4) & 0xFFFFFFFF)
    if op == 15:
        return 'lui %s,0x%x' % (R[rt], imm)
    if op in (8, 9, 10, 11):
        return '%s %s,%s,%d' % (OPS[op], R[rt], R[rs], simm)
    if op in (12, 13, 14):
        return '%s %s,%s,0x%x' % (OPS[op], R[rt], R[rs], imm)
    if op == 16:
        sub = rs
        if sub == 0:
            return 'mfc0 %s,$%d' % (R[rt], rd)
        if sub == 4:
            return 'mtc0 %s,$%d' % (R[rt], rd)
        if (x & 0x3F) == 0x10:
            return 'rfe'
        return 'cop0 %08x' % x
    if op == 18:
        if x & (1 << 25):
            return 'gte cmd %07x' % (x & 0x1FFFFFF)
        return '%s %s,$%d' % ({0: 'mfc2', 2: 'cfc2', 4: 'mtc2', 6: 'ctc2'}.get(rs, 'cop2?'), R[rt], rd)
    if op in OPS:
        return '%s %s,%d(%s)' % (OPS[op], R[rt], simm, R[rs])
    return 'op%d %08x' % (op, x)


def main():
    ram = open(sys.argv[1], 'rb').read()
    start = int(sys.argv[2], 16)
    count = int(sys.argv[3])
    base = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0
    for i in range(count):
        a = start + 4 * i
        o = (a & 0x1FFFFFFF) - base
        if o < 0 or o + 4 > len(ram):
            break
        x = struct.unpack('<I', ram[o:o + 4])[0]
        print('%08x: %08x  %s' % (a, x, dis(x, a)))


main()
