#!/usr/bin/env python3
"""
patch_qcsmmu_hupcf.py - Enable SCTLR.HUPCF=1 in Qualcomm qcsmmu8250.sys (xblfix)
Target: Xiaomi Pad 5 Pro (elish, SM8250-AC) under Windows 11 ARM64

Patch (1 ARM64 instruction + PE checksum):
  RVA 0x00a784 (file 0x009b84, .text, inside CB enable FUN_14000a5f8):
  08 79 17 12 (and w8, w8, #0xfffffeff) -> 08 01 18 32 (orr w8, w8, #0x100)
  Sets SCTLR.HUPCF=1 (bit 8) so a single transient GPU translation fault does
  not abort all subsequent transactions in the context bank (matching Linux/KGSL).
"""

import hashlib
import struct
import sys
from pathlib import Path

FILE_OFF = 0x009B84
RVA = 0x00A784
ORIG_HEX = "08791712"  # and w8, w8, #0xfffffeff
NEW_HEX = "08011832"   # orr w8, w8, #0x100


def calc_pe_checksum(data: bytearray, pe_offset: int) -> int:
    chk_offset = pe_offset + 0x58
    saved = data[chk_offset:chk_offset + 4]
    data[chk_offset:chk_offset + 4] = b"\x00\x00\x00\x00"
    chk = 0
    for (val,) in struct.iter_unpack("<I", bytes(data)):
        chk += val
        chk = (chk & 0xFFFFFFFF) + (chk >> 32)
    chk = (chk & 0xFFFF) + (chk >> 16)
    chk = (chk & 0xFFFF) + (chk >> 16)
    chk += len(data)
    chk &= 0xFFFFFFFF
    data[chk_offset:chk_offset + 4] = saved
    return chk


def main() -> None:
    if len(sys.argv) < 3:
        print("Usage: python patch_qcsmmu_hupcf.py <input_qcsmmu8250.sys> <output_patched.sys>")
        sys.exit(1)

    src_path = Path(sys.argv[1])
    dst_path = Path(sys.argv[2])
    data = bytearray(src_path.read_bytes())
    pe_offset = struct.unpack("<I", data[0x3C:0x40])[0]
    chk_offset = pe_offset + 0x58

    orig = bytes(data[FILE_OFF:FILE_OFF + 4])
    if orig != bytes.fromhex(ORIG_HEX):
        print(f"Error: mismatch at 0x{FILE_OFF:x} (RVA 0x{RVA:x}): got {orig.hex()}, expected {ORIG_HEX}")
        sys.exit(1)

    data[FILE_OFF:FILE_OFF + 4] = bytes.fromhex(NEW_HEX)
    new_chk = calc_pe_checksum(data, pe_offset)
    data[chk_offset:chk_offset + 4] = struct.pack("<I", new_chk)

    dst_path.write_bytes(data)
    print(f"[OK] Patched RVA 0x{RVA:06x} (file 0x{FILE_OFF:06x}): SCTLR.HUPCF=1")
    print(f"PE Checksum: 0x{new_chk:08x} | SHA1: {hashlib.sha1(data).hexdigest()}")


if __name__ == "__main__":
    main()
