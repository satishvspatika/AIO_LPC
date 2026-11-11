#!/usr/bin/env python3
"""Checks a hex file against the old SD bootloader V1.1's parser (TEST_JIG.md section 12):
it packs type 00 records in order into 4 KB blocks and takes each block's address from the
last record, so the data must be contiguous from 0x18000, in order, never crossing a 4 KB
boundary inside a record; only type 02 segment records; lines under 100 characters; one end
record, last; the image under 0x40000 (160 KB). Also simulates the parser and compares."""
import sys

START, END = 0x18000, 0x40000


def main(path):
    lines = [l.rstrip('\r\n') for l in open(path)]
    seg = 0
    expect = START
    data = bytearray()
    sim = {}
    block = bytearray(b'\xff' * 4096)
    fill = 0
    last_addr = 0
    ended = False
    for n, line in enumerate(lines, 1):
        if not line:
            continue
        if ended:
            sys.exit(f"[ERROR] line {n}: after the end record")
        if len(line) >= 99:
            sys.exit(f"[ERROR] line {n}: {len(line)} characters (old parser buffer is 100)")
        rec = bytes.fromhex(line[1:])
        if line[0] != ':' or sum(rec) & 0xFF or rec[0] != len(rec) - 5:
            sys.exit(f"[ERROR] line {n}: bad record")
        count, addr, rtype = rec[0], (rec[1] << 8) | rec[2], rec[3]
        if rtype == 2:
            seg = (rec[4] << 8) | rec[5]
        elif rtype == 0:
            full = seg * 16 + addr
            if full != expect:
                sys.exit(f"[ERROR] line {n}: data at 0x{full:05X}, expected 0x{expect:05X} (gap or out of order)")
            if (full % 4096) + count > 4096:
                sys.exit(f"[ERROR] line {n}: record crosses a 4 KB block")
            data += rec[4:4 + count]
            expect += count
            # the old parser
            block[fill:fill + count] = rec[4:4 + count]
            fill += count
            last_addr = addr
            if fill >= 4096:
                sim[seg * 16 + (last_addr // 4096) * 4096] = bytes(block)
                block = bytearray(b'\xff' * 4096)
                fill = 0
        elif rtype == 1 and count == 0:
            ended = True
            if fill:
                sim[seg * 16 + (last_addr // 4096) * 4096] = bytes(block)
        else:
            sys.exit(f"[ERROR] line {n}: record type {rtype} (the old parser knows 00, 01, 02 only)")
    if not ended:
        sys.exit("[ERROR] no end record: the old bootloader would hang")
    if expect > END:
        sys.exit(f"[ERROR] image ends at 0x{expect:05X}, over 0x{END:05X}: the old bootloader would hang")
    for i in range(0, len(data), 4096):
        want = bytes(data[i:i + 4096]).ljust(4096, b'\xff')
        if sim.get(START + i) != want:
            sys.exit(f"[ERROR] old parser simulation differs at block 0x{START + i:05X}")
    print(f"[OK] {path}: {len(data)} bytes 0x{START:05X}-0x{expect - 1:05X}, old bootloader V1.1 compatible")


if __name__ == '__main__':
    main(sys.argv[1])
