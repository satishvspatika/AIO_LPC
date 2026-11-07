#!/usr/bin/env python3
"""Fails the build if a merged hex is not safe to flash: bad checksums, overlaps, repeated start
addresses, a missing or misplaced end record, or data over the settings, staging or log areas."""
import sys

FORBIDDEN = [(0x01000, 0x02000, "settings"), (0x38000, 0x80000, "staging and record log")]
REQUIRED = [(0x00000, "boot stub"), (0x02000, "bootloader"), (0x0C000, "journal"), (0x18000, "firmware")]
FORBIDDEN_845 = [(0x01000, 0x02400, "journal and scratch"), (0x08400, 0x10000, "staging")]
REQUIRED_845 = [(0x00000, "bootloader"), (0x02400, "firmware")]
# The LPC845 reads this word as its code protection setting, and these values lock it for good
CRP_ADDR = 0x2FC
CRP_VALUES = (0x12345678, 0x87654321, 0x43218765, 0x4E697370)


def main(path, lpc845=False):
    forbidden = FORBIDDEN_845 if lpc845 else FORBIDDEN
    required = REQUIRED_845 if lpc845 else REQUIRED
    mem = {}
    base = 0
    ends = 0
    line_no = 0
    with open(path) as f:
        for line in f:
            line = line.strip()
            line_no += 1
            if not line:
                continue
            if ends:
                sys.exit(f"[ERROR] {path}:{line_no}: data after the end record")
            rec = bytes.fromhex(line[1:])
            if line[0] != ":" or sum(rec) & 0xFF or rec[0] != len(rec) - 5:
                sys.exit(f"[ERROR] {path}:{line_no}: bad record")
            rtype = rec[3]
            if rtype == 0:
                addr = base + ((rec[1] << 8) | rec[2])
                for i in range(rec[0]):
                    if addr + i in mem:
                        sys.exit(f"[ERROR] {path}:{line_no}: overlap at 0x{addr + i:05X}")
                    mem[addr + i] = rec[4 + i]
            elif rtype == 1:
                ends += 1
            elif rtype == 2:
                base = ((rec[4] << 8) | rec[5]) << 4
            elif rtype == 4:
                base = ((rec[4] << 8) | rec[5]) << 16
            else:
                sys.exit(f"[ERROR] {path}:{line_no}: unexpected record type {rtype}")
    if ends != 1:
        sys.exit(f"[ERROR] {path}: {ends} end records")
    for start, end, name in forbidden:
        if any(start <= a < end for a in mem):
            sys.exit(f"[ERROR] {path}: writes over the {name} area")
    for addr, name in required:
        if addr not in mem:
            sys.exit(f"[ERROR] {path}: no {name} at 0x{addr:05X}")
    if lpc845 and all(CRP_ADDR + i in mem for i in range(4)):
        word = sum(mem[CRP_ADDR + i] << (8 * i) for i in range(4))
        if word in CRP_VALUES:
            sys.exit(f"[ERROR] {path}: code at 0x{CRP_ADDR:03X} reads as code protection 0x{word:08X}")
    print(f"Hex OK: {len(mem)} bytes, no overlaps, protected areas untouched")


if __name__ == "__main__":
    main(sys.argv[1], len(sys.argv) > 2 and sys.argv[2] == "--lpc845")
