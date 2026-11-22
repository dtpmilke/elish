# Qualcomm Adreno 650 GPU for Xiaomi Pad 5 Pro (elish)

## Overview
Xiaomi Pad 5 Pro (`elish`) is powered by Snapdragon 870 (SM8250-AC), which features the Adreno 650 GPU and LPDDR5 memory. The stock Qualcomm Windows ARM64 graphics driver (`qcdxkm8250.sys`, WDDM 2.7) is targeted for Snapdragon 865 (SM8250 / Adreno 640 / LPDDR4X).

Without patching, four hardware/platform incompatibilities prevent stable GPU operation:
1. **GMU/PDC Cold Boot Hang**: The GPU Microcontroller (GMU) uses different TCM retention registers on Adreno 650 (`+0x1e730`) and requires PDC wake bit 31 (`0x80000001`).
2. **LPDDR5 Channel Hashing**: Snapdragon 870 with LPDDR5 requires `DDRHighestBankBit = 15` (stock driver hardcodes 16). Incorrect bank bit causes UBWC eviction translation faults in SMMU (UCHE) and ring hangs at fence 16.
3. **Permanent Black Screen / Display Deadlock (`p2a`)**: Stock `qcdx` v2.4 attempts 10nm D-PHY/PLL v3.0 re-init whenever the display is powered off or torn down during `DxgkDdiResetFromTimeout`, failing with `ERR=328(1)` on DSI1 and deadlocking `D+0x1098` / `win32k`. Preserving the UEFI-adopted 7nm C-PHY dual-DSI state across TDR and display power-off prevents the deadlock.
4. **Corrupted/Striped Font Glyphs (`p2b`)**: On non-coherent `elish`, unconditionally setting `AllocationInfoFlags.Cached` on GDI `STAGING_CPUVISIBLE` surfaces causes stale CPU-cached font glyphs when GPU reads from DRAM (`RVA 0x263ef0`).

## How to Patch
Run the patcher on the stock driver binary:
```bash
python patch_adreno650.py /path/to/stock/qcdxkm8250.sys qcdxkm8250.patched.sys
```

Then sign `qcdxkm8250.patched.sys` with your Authenticode test certificate:
```bash
osslsigncode sign -certs testcert.crt -key testkey.key -in qcdxkm8250.patched.sys -out qcdxkm8250.signed.sys
```

## Registry Settings
Apply the following parameters to `HKLM\SOFTWARE\QCOM\Drivers\DxKMD`:
- `DDRHighestBankBit` = `dword:0000000f` (15)
- `UBWCEnable` = `dword:00000001` (1)
- `GmemSize` = `dword:00120000` (1179648 bytes / 1.125 MB)
- `DISPLAY\DisableExternal` = `dword:00000004` (4 — disables phantom second display target, stops `0x4fa` modality storm)
- `GRAPHICS\FlushTLBOnEverySubmit` = `dword:00000001` (1 — prevents stale SMMU TLB entries on same-root submits)

Apply to `HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers`:
- `TdrLevel` = `dword:00000003` (3 — standard TDR recovery)
- `Scheduler\EnablePreemption` = `dword:00000000` (0 — avoids A690 preemption save/restore mismatches on A650)

Set service start to Demand / Hardware start:
- `HKLM\SYSTEM\CurrentControlSet\Services\QCDX\Start` = `dword:00000003`

## Panel Configuration
Generate the K81 panel XML using `gen_panel_xml.py` to match the 1600x2560 @ 120 Hz internal display.
