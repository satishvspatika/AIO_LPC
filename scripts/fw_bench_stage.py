#!/usr/bin/env python3
"""BENCH ONLY: makes a hex that puts an image in the staging slot and marks it pending, so the
bootloader can be tested with MCU-Link before the SD update exists.
  fw_bench_stage.py <image.bin> <out.hex>
Then flash with: pyocd flash -t lpc1768 -f 1000000 --erase sector <out.hex>"""
import struct
import sys
import time
import zlib

SLOT_B_BASE = 0x38000
JOURNAL_PAGE_ADDR = 0xD000  # Start of a journal sector
J_MAGIC = 0x4C4E524A
J_PENDING = 1


def hex_records(addr, data):
    lines = []
    upper = None
    for i in range(0, len(data), 32):
        chunk = data[i:i + 32]
        a = addr + i
        if (a >> 16) != upper:
            upper = a >> 16
            rec = bytes([2, 0, 0, 4]) + struct.pack(">H", upper)
            lines.append(":" + (rec + bytes([(-sum(rec)) & 0xFF])).hex().upper())
        rec = bytes([len(chunk), (a >> 8) & 0xFF, a & 0xFF, 0]) + chunk
        lines.append(":" + (rec + bytes([(-sum(rec)) & 0xFF])).hex().upper())
    return lines


def main(image_path, out_path):
    with open(image_path, "rb") as f:
        image = f.read()
    # Newest record wins, so a time-based sequence beats anything already in the journal
    head = struct.pack("<IIBBBB", J_MAGIC, int(time.time()), J_PENDING, 0, 0, 0xFF)
    record = head + struct.pack("<I", zlib.crc32(head) & 0xFFFFFFFF)
    page = record + b"\xFF" * (256 - len(record))
    with open(out_path, "w") as f:
        for line in hex_records(SLOT_B_BASE, image) + hex_records(JOURNAL_PAGE_ADDR, page):
            f.write(line + "\n")
        f.write(":00000001FF\n")
    print(f"Wrote {out_path}: {len(image)} byte image at 0x{SLOT_B_BASE:05X}, pending record at 0x{JOURNAL_PAGE_ADDR:05X}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
