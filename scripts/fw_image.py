#!/usr/bin/env python3
"""Fills in an image's length and CRC32 after linking. Reserved vector 8 points to its info block."""
import struct
import sys
import zlib

SLOT_A_BASE = 0x18000
SLOT_SIZE = 0x20000
MAGIC = 0x31474D49
BOARD = 0x1768


def main(path):
    with open(path, "rb") as f:
        data = bytearray(f.read())
    if len(data) > SLOT_SIZE:
        sys.exit(f"[ERROR] Image is {len(data)} bytes: over the {SLOT_SIZE} byte firmware slot")
    info_addr = struct.unpack_from("<I", data, 8 * 4)[0]
    off = info_addr - SLOT_A_BASE
    if off < 0 or off + 36 > len(data):
        sys.exit(f"[ERROR] Vector 8 (0x{info_addr:08X}) does not point inside the image")
    magic, board = struct.unpack_from("<II", data, off)
    if magic != MAGIC or board != BOARD:
        sys.exit(f"[ERROR] No image info block at 0x{info_addr:08X}")
    struct.pack_into("<I", data, off + 8, len(data))
    struct.pack_into("<I", data, off + 12, 0)
    crc = zlib.crc32(bytes(data)) & 0xFFFFFFFF
    struct.pack_into("<I", data, off + 12, crc)
    with open(path, "wb") as f:
        f.write(data)
    version = bytes(data[off + 20:off + 36]).split(b"\0")[0].decode("ascii", "replace")
    print(f"Image {version}: {len(data)} bytes of {SLOT_SIZE}, CRC32 0x{crc:08X}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("Usage: fw_image.py <image.bin>")
    main(sys.argv[1])
