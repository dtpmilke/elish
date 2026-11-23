#!/usr/bin/env python3
"""
patch_adreno650.py - Universal patcher for Qualcomm Adreno 650 (SM8250-AC / LPDDR5)
Target: Xiaomi Pad 5 Pro (elish) under Windows 11 ARM64

Applies all 27 hardware & stability patches:
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
6. Stage 6 (p2j — Fix Windows Internal Display Brightness Slider, D3DKMDT_VOT_INTERNAL & Sync):
   - BacklightInit_SkipBit23: NOP bit 23 check in display init to allow QCDXDL_EVT_INIT
   - BacklightNotify_SkipBit23: NOP bit 23 check in notify path to allow QCDXDL_BACKLIGHT_CHANGE
   - QueryInterface2_AlwaysSupported: always return DXGK_BRIGHTNESS_INTERFACE_2 table to Dxgkrnl
   - SetBrightnessState2_StatusOk: return STATUS_SUCCESS instead of failing on absent PMIC
   - GetBrightness2_StatusOk / RetBranch: bypass failing I2C backend read and return STATUS_SUCCESS with cached brightness
   - GetBacklightReduction_Ratio: return 100% Backlight ratio instead of zero to prevent Windows dimming
   - SetBrightness2_StatusOk: return STATUS_SUCCESS instead of failing on absent PMIC I2C backend (fixes slider bounce)
   - SetBrightness3_StatusOk: return STATUS_SUCCESS instead of failing on absent PMIC I2C backend
   - GetBrightness3_SyncPercent: return valid millinits synced with percentage (eliminates 0-nits clamp)
   - Child0_InternalVideoOutputTech: set D3DKMDT_VOT_INTERNAL (0x80000000) instead of HDMI (5) for Child 0 (internal panel)
   - Child0_InternalVideoOutputTech_Str: always store D3DKMDT_VOT_INTERNAL for Child 0 (eliminates "external monitor" fallback)
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
        'name': 'BacklightInit_SkipBit23',
        'rva': 0x0b0580,
        'file_off': 0x0aeb80,
        'old': bytes.fromhex('6801b836'),
        'new': bytes.fromhex('1f2003d5'),
        'desc': 'NOP tbz w8, #0x17 in display init (allow QCDXDL_EVT_INIT to register callback and display)'
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
        'name': 'QueryInterface2_AlwaysSupported',
        'rva': 0x269ab4,
        'file_off': 0x1d5ab4,
        'old': bytes.fromhex('29010037'),
        'new': bytes.fromhex('09000014'),
        'desc': 'b #0x269ad8 (always return DXGK_BRIGHTNESS_INTERFACE_2 table to Dxgkrnl)'
    },
    {
        'name': 'SetBrightnessState2_StatusOk',
        'rva': 0x2bf158,
        'file_off': 0x22b158,
        'old': bytes.fromhex('60023fd6f303002a'),
        'new': bytes.fromhex('1300805210000014'),
        'desc': 'mov w19, #0; b #0x40 to 0x2bf19c (SetBrightnessState2 returns STATUS_SUCCESS)'
    },
    {
        'name': 'GetBrightness2_StatusOk',
        'rva': 0x2bf284,
        'file_off': 0x22b284,
        'old': bytes.fromhex('60023fd6'),
        'new': bytes.fromhex('13008052'),
        'desc': 'mov w19, #0 (GetBrightness2 returns STATUS_SUCCESS; bypass failing I2C backend read)'
    },
    {
        'name': 'GetBrightness2_RetBranch',
        'rva': 0x2bf288,
        'file_off': 0x22b288,
        'old': bytes.fromhex('f303002a'),
        'new': bytes.fromhex('0e000014'),
        'desc': 'b #0x38 to 0x2bf2c0 (read cached brightness from [disp + 0x251]; prevents slider bounce to 0)'
    },
    {
        'name': 'GetBacklightReduction_Ratio',
        'rva': 0x2bf2cc,
        'file_off': 0x22b2cc,
        'old': bytes.fromhex('89020035bf0600b9'),
        'new': bytes.fromhex('a90600b913000014'),
        'desc': 'str w9, [x21, #4]; b #0x4c to 0x2bf31c (return 100% Backlight ratio instead of zero)'
    },
    {
        'name': 'SetBrightness2_StatusOk',
        'rva': 0x2bf3dc,
        'file_off': 0x22b3dc,
        'old': bytes.fromhex('60023fd6f303002a'),
        'new': bytes.fromhex('1300805210000014'),
        'desc': 'mov w19, #0; b #0x40 to 0x2bf420 (SetBrightness2 returns STATUS_SUCCESS; fixes slider bounce)'
    },
    {
        'name': 'SetBrightness3_StatusOk',
        'rva': 0x2bf5dc,
        'file_off': 0x22b5dc,
        'old': bytes.fromhex('60023fd6f303002a'),
        'new': bytes.fromhex('1300805210000014'),
        'desc': 'mov w19, #0; b #0x40 to 0x2bf620 (SetBrightness3 returns STATUS_SUCCESS)'
    },
    {
        'name': 'GetBrightness3_SyncPercent',
        'rva': 0x2bf6bc,
        'file_off': 0x22b6bc,
        'old': bytes.fromhex('2a5d42b9aa0200b9888947f9e9220d9b348141f9880240f9138140f948f4fff008c942f9'),
        'new': bytes.fromhex('2a5d42b98a0000352a4549390b7182524a7d0b1baa0200b9ca0200b91300805221000014'),
        'desc': 'sync millinits with percentage (ldrb w10, [x9, #0x251]; mul w10, 5000; returns STATUS_SUCCESS)'
    },
    {
        'name': 'Child0_InternalVideoOutputTech_Restore',
        'rva': 0x295030,
        'file_off': 0x201030,
        'old': (bytes.fromhex('880200b9'), bytes.fromhex('c4010314')),
        'new': bytes.fromhex('880200b9'),
        'desc': 'str w8, [x20] (restore stock Child 0 descriptor; eliminate s4a detour to HDMI)'
    },
    {
        'name': 'Child0_InternalVideoOutputTech_Str_Restore',
        'rva': 0x295058,
        'file_off': 0x201058,
        'old': (bytes.fromhex('990600b9'), bytes.fromhex('c2010314')),
        'new': bytes.fromhex('990600b9'),
        'desc': 'str w25, [x20, #4] (restore stock D3DKMDT_VOT_INTERNAL 0x80000000 for Child 0; eliminate s4a HDMI detour)'
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
        expected_olds = p['old'] if isinstance(p['old'], (tuple, list)) else (p['old'],)
        length = len(expected_olds[0])
        actual = bytes(data[off:off+length])
        if actual not in expected_olds:
            expected_str = '/'.join(o.hex() for o in expected_olds)
            print(f"Error: Patch '{p['name']}' mismatch at offset 0x{off:x}! Expected {expected_str}, found {actual.hex()}")
            sys.exit(1)
        data[off:off+length] = p['new']
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
