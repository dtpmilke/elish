#!/usr/bin/env python3
"""
patch_adreno650.py - Universal patcher for Qualcomm Adreno 650 (SM8250-AC / LPDDR5)
Target: Xiaomi Pad 5 Pro (elish) under Windows 11 ARM64

Applies all 6 hardware patches:
1. Stage 1 (GMU/PDC Cold Boot):
   - GENERAL_7_start: Direct branch to A650 TCM retention path (+0x1e730)
   - GENERAL_7_stop_selector: S/R literal offset +0x1e730
   - GENERAL_7_retention_persist: S/R retention persistence (retention=1)
   - ENABLE_PDC_bit31_mov: Master PDC wake bit 31 (0x80000001)
   - ENABLE_PDC_bit31_str: Direct store to [x8+x10]
2. Stage 2 (LPDDR5 Channel Hashing / Hang Detect):
   - DDRHighestBankBit: mov w9, #0x400f (highest bank bit = 15 for LPDDR5)
"""

import sys
import os
import struct
import hashlib
from pathlib import Path

PATCHES = [
    {
        'name': 'GENERAL_7_start',
        'rva': 0x2bc6d0,
        'file_off': 0x2286d0,
        'old': bytes.fromhex('81000054'),
        'new': bytes.fromhex('04000014'),
        'desc': 'b.ne #0x2bc6e0 -> b #0x2bc6e0 (Start uses A650 GENERAL_7 path)'
    },
    {
        'name': 'GENERAL_7_stop_selector',
        'rva': 0x22360,
        'file_off': 0x20960,
        'old': bytes.fromhex('00e10300'),
        'new': bytes.fromhex('30e70100'),
        'desc': 'literal 0x3e100 -> 0x1e730 (Stop selector uses A650 GENERAL_7 offset)'
    },
    {
        'name': 'GENERAL_7_retention_persist',
        'rva': 0x22000,
        'file_off': 0x20600,
        'old': bytes.fromhex('3f6928b8'),
        'new': bytes.fromhex('386928b8'),
        'desc': 'str wzr, [x9, x8] -> str w24, [x9, x8] (S/R stop keeps retention=1)'
    },
    {
        'name': 'ENABLE_PDC_bit31_mov',
        'rva': 0x2bc4bc,
        'file_off': 0x2284bc,
        'old': bytes.fromhex('091440f9'),
        'new': bytes.fromhex('ec070132'),
        'desc': 'ldr x9, [x0, #0x28] -> mov w12, #0x80000001'
    },
    {
        'name': 'ENABLE_PDC_bit31_str',
        'rva': 0x2bc4c4,
        'file_off': 0x2284c4,
        'old': bytes.fromhex('2c692ab8'),
        'new': bytes.fromhex('0c692ab8'),
        'desc': 'str w12, [x9, x10] -> str w12, [x8, x10]'
    },
    {
        'name': 'DDRHighestBankBit_LPDDR5',
        'rva': 0x293b38,
        'file_off': 0x1ffb38,
        'old': bytes.fromhex('09028852'),
        'new': bytes.fromhex('e9018852'),
        'desc': 'mov w9, #0x4010 -> mov w9, #0x400f (DDRHighestBankBit = 15 for LPDDR5)'
    }
]

def calc_pe_checksum(data: bytearray, pe_offset: int) -> int:
    chk_offset = pe_offset + 0x58
    saved = data[chk_offset:chk_offset+4]
    data[chk_offset:chk_offset+4] = b'\x00\x00\x00\x00'
    chk = 0
    for (val,) in struct.iter_unpack('<I', bytes(data)):
        chk += val
        chk = (chk & 0xffffffff) + (chk >> 32)
    chk = (chk & 0xffff) + (chk >> 16)
    chk = (chk & 0xffff) + (chk >> 16)
    chk += len(data)
    chk &= 0xffffffff
    data[chk_offset:chk_offset+4] = saved
    return chk

def main():
    if len(sys.argv) < 3:
        print("Usage: python patch_adreno650.py <input_stock_qcdxkm8250.sys> <output_patched.sys>")
        sys.exit(1)

    src_path = Path(sys.argv[1])
    dst_path = Path(sys.argv[2])

    if not src_path.exists():
        print(f"Error: Source file '{src_path}' not found.")
        sys.exit(1)

    data = bytearray(src_path.read_bytes())
    pe_offset = struct.unpack('<I', data[0x3c:0x40])[0]
    chk_offset = pe_offset + 0x58

    print(f"Input file: {src_path} ({len(data)} bytes, SHA1={hashlib.sha1(data).hexdigest()})")
    print("Applying hardware patches for Adreno 650 & LPDDR5...")

    for p in PATCHES:
        off = p['file_off']
        actual = data[off:off+4]
        if actual != p['old']:
            print(f"Error: Patch '{p['name']}' mismatch at offset 0x{off:x}! Expected {p['old'].hex()}, found {actual.hex()}")
            sys.exit(1)
        data[off:off+4] = p['new']
        print(f"  [OK] 0x{p['rva']:06x} (file 0x{off:06x}): {p['name']} -> {p['desc']}")

    # Update PE Checksum
    new_chk = calc_pe_checksum(data, pe_offset)
    data[chk_offset:chk_offset+4] = struct.pack('<I', new_chk)
    print(f"PE Checksum recalculated: 0x{new_chk:08x}")

    dst_path.write_bytes(data)
    out_sha1 = hashlib.sha1(data).hexdigest()
    print(f"\nSuccessfully wrote: {dst_path}")
    print(f"Output SHA1: {out_sha1}")
    print("Note: Sign this binary with Authenticode (e.g. via osslsigncode or signtool) before loading into Windows Kernel.")

if __name__ == '__main__':
    main()
