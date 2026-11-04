#!/usr/bin/env python3
"""
LPC1768 Vector Table Checksum Calculator & Patcher
Computes 2's complement checksum of Cortex-M3 vectors 0..6 and writes it to vector 7 (offset 0x1C).
NXP LPC microcontrollers & bootloaders require this checksum to consider firmware valid.
"""
import sys
import struct

def patch_lpc_checksum(bin_path):
    with open(bin_path, "rb+") as f:
        data = bytearray(f.read())
        if len(data) < 32:
            print(f"Error: {bin_path} is too small to be a valid LPC binary.")
            sys.exit(1)
        
        # Extract vectors 0..6 (first 28 bytes)
        words = struct.unpack("<7I", data[0:28])
        
        # Calculate 2's complement checksum of vectors 0..6
        checksum = (-sum(words)) & 0xFFFFFFFF
        
        # Write checksum to vector 7 (offset 0x1C / 28)
        struct.pack_into("<I", data, 28, checksum)
        
        f.seek(0)
        f.write(data)
        print(f"Successfully patched LPC1768 Vector 7 Checksum (0x{checksum:08X}) into {bin_path}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: lpc_checksum.py <path_to_binary.bin>")
        sys.exit(1)
    patch_lpc_checksum(sys.argv[1])
