#!/usr/bin/env python3
"""Fills in an image's length and CRC32 after linking. Reserved vector 8 points to its info block.
On the LPC845 the prebuilt startup can't set that vector, so this does: the linker script puts the
info block straight after the vector table."""
import struct
import sys
import zlib

SLOT_A_BASE = 0x18000
SLOT_SIZE = 0x20000
MAGIC = 0x31474D49
BOARD = 0x1768


def main(path, lpc845=False):
    slot_a_base = 0x2400 if lpc845 else SLOT_A_BASE
    slot_size = 0x6000 if lpc845 else SLOT_SIZE
    board_id = 0x845 if lpc845 else BOARD
    with open(path, "rb") as f:
        data = bytearray(f.read())
    if len(data) > slot_size:
        sys.exit(f"[ERROR] Image is {len(data)} bytes: over the {slot_size} byte firmware slot")
    if lpc845:
        struct.pack_into("<I", data, 8 * 4, slot_a_base + 0xC0)
    info_addr = struct.unpack_from("<I", data, 8 * 4)[0]
    off = info_addr - slot_a_base
    if off < 0 or off + 36 > len(data):
        sys.exit(f"[ERROR] Vector 8 (0x{info_addr:08X}) does not point inside the image")
    magic, board = struct.unpack_from("<II", data, off)
    if magic != MAGIC or board != board_id:
        sys.exit(f"[ERROR] No image info block at 0x{info_addr:08X}")
    struct.pack_into("<I", data, off + 8, len(data))
    struct.pack_into("<I", data, off + 12, 0)
    crc = zlib.crc32(bytes(data)) & 0xFFFFFFFF
    struct.pack_into("<I", data, off + 12, crc)
    with open(path, "wb") as f:
        f.write(data)
    version = bytes(data[off + 20:off + 36]).split(b"\0")[0].decode("ascii", "replace")
    print(f"Image {version}: {len(data)} bytes of {slot_size}, CRC32 0x{crc:08X}")
    if lpc845:
        return
    # What the SD card file for this image is called
    profile = struct.unpack_from("<I", data, off + 16)[0]
    names = {0: "fw_ksn_trg.hex", 1: "fw_bihar.hex", 2: "fw_ksn_tws.hex"}
    with open(path + ".sdname", "w") as f:
        f.write(names[profile])


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and sys.argv[2] != "--lpc845"):
        sys.exit("Usage: fw_image.py <image.bin> [--lpc845]")
    main(sys.argv[1], len(sys.argv) == 3)
