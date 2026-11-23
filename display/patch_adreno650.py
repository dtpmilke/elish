#!/usr/bin/env python3
"""
patch_adreno650.py - Universal patcher for Qualcomm Adreno 650 (SM8250-AC / LPDDR5)
Target: Xiaomi Pad 5 Pro (elish) under Windows 11 ARM64

Applies all 24 hardware & stability patches:
1. Stage 1 (GMU/PDC Cold Boot):
   - GENERAL_7_start: Direct branch to A650 TCM retention path (+0x1e730)
   - GENERAL_7_stop_selector: S/R literal offset +0x1e730
   - GENERAL_7_retention_persist: S/R retention persistence (retention=1)
   - ENABLE_PDC_bit31_mov: Master PDC wake bit 31 (0x80000001)
   - ENABLE_PDC_bit31_str: Direct store to [x8+x10]
2. Stage 2 (LPDDR5 Channel Hashing / Hang Detect):
   - DDRHighestBankBit: mov w9, #0x400f (highest bank bit = 15 for LPDDR5)
3. Stage 3 (p2a — Prevent Permanent Black Screen / D+0x1098 Display Deadlock):
   - MDPSetDisplayPower_NoOff: keep UEFI 7nm C-PHY dual-DSI state intact on power-off
   - ResetFromTimeout_SkipDispPwr: skip DISPLAY PoFx/GDSC power-cycle on GPU TDR
   - ResetFromTimeout_SkipDispTeardown: skip display target teardown loop on GPU TDR
4. Stage 4 (p2b/p2c — Fix Corrupted/Striped Font Glyphs on Non-Coherent Platform):
   - GdiStaging_NoCached: clear forced Cached flag on GDI STAGING_CPUVISIBLE allocations
   - CreateAllocation_NoCached: skip setting AllocationInfoFlags.Cached for all allocations (UMD Direct2D/DirectWrite + GDI)
5. Stage 5 (p2d/p2e — Match Android KGSL / Linux Mainline A650 hw_init):
   - UCHE_GMEM_RANGE_MIN_LO/HI, MAX_LO/HI: NOP 4 stores (not programmed on A650)
   - GBIF_QSB_SIDE0..3: 0x50620 -> 0x71620, RBBM_GBIF_CLIENT_QOS_CNTL: 0 -> 3, drop 0x5ff
   - PC_DBG_ECO_CNTL (0x9e00): 0x800200 -> 0x300200
   - NOP A660/A690-only 0x9b2 and UCHE_CMDQ_CONFIG (0x2b51d4, 0x2b51dc)
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
    },
    {
        'name': 'MDPSetDisplayPower_NoOff',
        'rva': 0x099f60,
        'file_off': 0x098560,
        'old': bytes.fromhex('28c12c91'),
        'new': bytes.fromhex('87000014'),
        'desc': 'MDPSetDisplayPower(powerOn=0) -> b 0x14009a17c (return 0, preserve UEFI 7nm C-PHY DSI state)'
    },
    {
        'name': 'ResetFromTimeout_SkipDispPwr',
        'rva': 0x2980c0,
        'file_off': 0x2040c0,
        'old': bytes.fromhex('b8010034'),
        'new': bytes.fromhex('0d000014'),
        'desc': 'DxgkDdiResetFromTimeout -> b 0x1402980f4 (skip DISPLAY 0x26b5c0 PoFx/GDSC power-cycle)'
    },
    {
        'name': 'ResetFromTimeout_SkipDispTeardown',
        'rva': 0x297e94,
        'file_off': 0x203e94,
        'old': bytes.fromhex('c8010034'),
        'new': bytes.fromhex('0e000014'),
        'desc': 'DxgkDdiResetFromTimeout -> b 0x140297ecc (skip 0x2c6198/0x2c64b8 display target teardown)'
    },
    {
        'name': 'GdiStaging_NoCached',
        'rva': 0x263ef0,
        'file_off': 0x1cfef0,
        'old': bytes.fromhex('09011932'),
        'new': bytes.fromhex('e903082a'),
        'desc': 'orr w9, w8, #0x80 -> mov w9, w8 (do not force Cached on GDI STAGING_CPUVISIBLE)'
    },
    {
        'name': 'CreateAllocation_NoCached',
        'rva': 0x263618,
        'file_off': 0x1cf618,
        'old': bytes.fromhex('a8000037'),
        'new': bytes.fromhex('05000014'),
        'desc': 'tbnz w8, #0, #0x26362c -> b #0x26362c (skip AllocationInfoFlags.Cached for all UMD+GDI allocations; fixes Start/Search font stripes)'
    },
    {
        'name': 'UCHE_GMEM_RANGE_MIN_LO_NOP',
        'rva': 0x2b539c,
        'file_off': 0x22139c,
        'old': bytes.fromhex('0b2d38b9'),
        'new': bytes.fromhex('1f2003d5'),
        'desc': 'NOP str w11, [x8, #0x382c] (A650 does not program UCHE_GMEM_RANGE)'
    },
    {
        'name': 'UCHE_GMEM_RANGE_MIN_HI_NOP',
        'rva': 0x2b53ac,
        'file_off': 0x2213ac,
        'old': bytes.fromhex('0a3138b9'),
        'new': bytes.fromhex('1f2003d5'),
        'desc': 'NOP str w10, [x8, #0x3830] (A650 does not program UCHE_GMEM_RANGE)'
    },
    {
        'name': 'UCHE_GMEM_RANGE_MAX_LO_NOP',
        'rva': 0x2b53bc,
        'file_off': 0x2213bc,
        'old': bytes.fromhex('2c3538b9'),
        'new': bytes.fromhex('1f2003d5'),
        'desc': 'NOP str w12, [x9, #0x3834] (A650 does not program UCHE_GMEM_RANGE)'
    },
    {
        'name': 'UCHE_GMEM_RANGE_MAX_HI_NOP',
        'rva': 0x2b53c4,
        'file_off': 0x2213c4,
        'old': bytes.fromhex('283938b9'),
        'new': bytes.fromhex('1f2003d5'),
        'desc': 'NOP str w8, [x9, #0x3838] (A650 does not program UCHE_GMEM_RANGE)'
    },
    {
        'name': 'GBIF_QSB_SIDE0',
        'rva': 0x14d584,
        'file_off': 0x14b984,
        'old': bytes.fromhex('20060500'),
        'new': bytes.fromhex('20160700'),
        'desc': 'GBIF_QSB_SIDE0: 0x50620 -> 0x71620 (KGSL A650)'
    },
    {
        'name': 'GBIF_QSB_SIDE1',
        'rva': 0x14d58c,
        'file_off': 0x14b98c,
        'old': bytes.fromhex('20060500'),
        'new': bytes.fromhex('20160700'),
        'desc': 'GBIF_QSB_SIDE1: 0x50620 -> 0x71620 (KGSL A650)'
    },
    {
        'name': 'GBIF_QSB_SIDE2',
        'rva': 0x14d594,
        'file_off': 0x14b994,
        'old': bytes.fromhex('20060500'),
        'new': bytes.fromhex('20160700'),
        'desc': 'GBIF_QSB_SIDE2: 0x50620 -> 0x71620 (KGSL A650)'
    },
    {
        'name': 'GBIF_QSB_SIDE3',
        'rva': 0x14d59c,
        'file_off': 0x14b99c,
        'old': bytes.fromhex('20060500'),
        'new': bytes.fromhex('20160700'),
        'desc': 'GBIF_QSB_SIDE3: 0x50620 -> 0x71620 (KGSL A650)'
    },
    {
        'name': 'RBBM_GBIF_CLIENT_QOS_CNTL',
        'rva': 0x14d5a4,
        'file_off': 0x14b9a4,
        'old': bytes.fromhex('00000000'),
        'new': bytes.fromhex('03000000'),
        'desc': 'RBBM_GBIF_CLIENT_QOS_CNTL: 0 -> 3 (KGSL A650)'
    },
    {
        'name': 'GBIF_TABLE_TERMINATE',
        'rva': 0x14d5a8,
        'file_off': 0x14b9a8,
        'old': bytes.fromhex('ff050000'),
        'new': bytes.fromhex('00000000'),
        'desc': 'Drop A690-only 0x5ff entry (terminate GBIF table)'
    },
    {
        'name': 'PC_DBG_ECO_CNTL',
        'rva': 0x14d5ec,
        'file_off': 0x14b9ec,
        'old': bytes.fromhex('00028000'),
        'new': bytes.fromhex('00023000'),
        'desc': 'PC_DBG_ECO_CNTL (0x9e00): 0x800200 -> 0x300200 (KGSL A650)'
    },
    {
        'name': 'A690_ONLY_0x9B2_NOP',
        'rva': 0x2b51d4,
        'file_off': 0x2211d4,
        'old': bytes.fromhex('4ec926b9'),
        'new': bytes.fromhex('1f2003d5'),
        'desc': 'NOP str w14, [x10, #0x26c8] (A690-only 0x9b2)'
    },
    {
        'name': 'A660_ONLY_UCHE_CMDQ_NOP',
        'rva': 0x2b51dc,
        'file_off': 0x2211dc,
        'old': bytes.fromhex('6cf138b9'),
        'new': bytes.fromhex('1f2003d5'),
        'desc': 'NOP str w12, [x11, #0x38f0] (A660/A690-only UCHE_CMDQ_CONFIG)'
    },
    {
        'name': 'BacklightNotify_SkipBit23',
        'rva': 0x0e2a50,
        'file_off': 0x0e1050,
        'old': bytes.fromhex('c90ab836'),
        'new': bytes.fromhex('1f2003d5'),
        'desc': 'NOP tbz w9, #0x17 in backlight notify path (allow QCDXDL_BACKLIGHT_CHANGE)'
    },
    {
        'name': 'GetBrightness3_CopyTarget',
        'rva': 0x2bf6c4,
        'file_off': 0x22b6c4,
        'old': bytes.fromhex('888947f9'),
        'new': bytes.fromhex('ca0200b9'),
        'desc': 'str w10, [x22] (GetBrightness3 returns Current = Target)'
    },
    {
        'name': 'GetBrightness3_StatusOk',
        'rva': 0x2bf6c8,
        'file_off': 0x22b6c8,
        'old': bytes.fromhex('e9220d9b'),
        'new': bytes.fromhex('13008052'),
        'desc': 'mov w19, #0 (GetBrightness3 returns STATUS_SUCCESS)'
    },
    {
        'name': 'GetBrightness3_RetBranch',
        'rva': 0x2bf6cc,
        'file_off': 0x22b6cc,
        'old': bytes.fromhex('348141f9'),
        'new': bytes.fromhex('25000014'),
        'desc': 'b #0x94 to epilogue (skip failing backend +0x100 read)'
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
