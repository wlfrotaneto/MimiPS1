#!/usr/bin/env python3
"""Reports how much of a PS-X EXE's text segment the recompiler emitted code for.

usage: cfg_coverage.py <boot.exe> <recompiled_game.c>

Uncovered runs are classified as code-like or data-like with a cheap heuristic
(share of words that decode as valid MIPS-I opcodes), so gaps that matter stand out.
"""
import re
import struct
import sys


def valid_word(w):
    op = w >> 26
    if op in (0x10, 0x11, 0x12, 0x13):          # COPz
        return True
    if op == 0:
        return (w & 0x3F) in (0, 2, 3, 4, 6, 7, 8, 9, 12, 13, 16, 17, 18, 19, 24, 25, 26, 27, 32, 33, 34, 35, 36, 37, 38, 39, 42, 43)
    if op == 1:
        return ((w >> 16) & 0x1F) in (0, 1, 16, 17)
    return op in (2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 32, 33, 34, 35, 36, 37, 38, 40, 41, 42, 43, 46, 50, 58)


def main():
    exe = open(sys.argv[1], 'rb').read()
    src = open(sys.argv[2]).read()
    base = struct.unpack('<I', exe[0x18:0x1c])[0] | 0x80000000
    size = struct.unpack('<I', exe[0x1c:0x20])[0]
    words = struct.unpack('<%dI' % (size // 4), exe[0x800:0x800 + size])
    locs = {int(x, 16) | 0x80000000 for x in re.findall(r'^loc_([0-9a-f]{8}):', src, re.M)}

    n = size // 4
    covered = sum(1 for i in range(n) if (base + 4 * i) in locs)
    print("text 0x%08X..0x%08X  insns=%d  covered=%d (%.1f%%)" % (base, base + size, n, covered, 100.0 * covered / n))

    runs, start = [], None
    for i in range(n + 1):
        unc = i < n and (base + 4 * i) not in locs
        if unc and start is None:
            start = i
        elif not unc and start is not None:
            runs.append((start, i))
            start = None
    code_like = 0
    for s, e in sorted(runs, key=lambda r: r[0] - r[1]):
        ws = words[s:e]
        score = sum(1 for w in ws if w != 0 and valid_word(w)) / max(1, sum(1 for w in ws if w != 0))
        kind = "code-like" if score > 0.97 and any(w == 0x03E00008 for w in ws) else "data-like"
        if kind == "code-like":
            code_like += e - s
        if e - s >= 8:
            print("  gap 0x%08X..0x%08X %6d insns  %s (%.0f%% valid)" % (base + 4 * s, base + 4 * e, e - s, kind, 100 * score))
    print("uncovered code-like insns: %d" % code_like)


main()
