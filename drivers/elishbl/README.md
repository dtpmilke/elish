# Backlight Control for Xiaomi Pad 5 Pro (elish)

## Overview
Xiaomi Pad 5 Pro uses the Kinetic KTZ8866 backlight LED driver connected over I2C (via GPIO60 / GPIO61 bitbang / ACPI).

Because the Adreno 650 GPU driver (`qcdxkm8250.sys`) does not have direct ACPI I2C access on Windows ARM64, the brightness control architecture is split into three clean layers:

1. **GPU Miniport Patch (`patch_adreno650.py` — `p2g`)**:
   - Implements `DXGK_BRIGHTNESS_INTERFACE_2` (`GetBrightness2`).
   - Bypasses the failing hardware I2C read and returns `STATUS_SUCCESS` along with the driver's cached brightness level from `[disp + 0x251]`.
   - Prevents Windows 11 Settings slider from resetting / bouncing back to 0%.

2. **WMI Brightness Sync Service (`ElishBrightnessSync.cs`)**:
   - Lightweight C# background service (~7.5 KB binary, zero third-party dependencies).
   - Subscribes to Windows `root\wmi:WmiMonitorBrightnessEvent`.
   - Whenever the user moves the brightness slider in **Settings → System → Display** or Quick Settings, receives the event (debounced 60 ms) and commits the percentage to `HKLM\SOFTWARE\elish\Brightness`.

3. **Kernel Backlight Driver (`elishbl.sys`)**:
   - Native Windows kernel-mode driver (`elishbl.c`).
   - Monitors `HKLM\SOFTWARE\elish\Brightness` via `ZwNotifyChangeKey`.
   - Maps 0–100% brightness to 11-bit KTZ8866 raw values (0–2047) using a balanced curve (50% linear + 50% quadratic).
   - Tracks display power states (`GUID_CONSOLE_DISPLAY_STATE` and `GUID_MONITOR_POWER_ON` via `PoRegisterPowerSettingCallback`) and system sleep/wake (`\Callback\PowerState`) to cleanly turn off LEDs on screen-off and restore brightness on screen-on.

## Building ElishBrightnessSync
Using the standard Windows built-in C# compiler:
```cmd
C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe /target:winexe /out:ElishBrightnessSync.exe ElishBrightnessSync.cs /r:System.Management.dll
```

## Installation
1. Install `elishbl.sys` as a system service.
2. Register `ElishBrightnessSync.exe` as a scheduled task running at system boot:
```cmd
schtasks /Create /TN "\elish-brightness-sync" /TR "C:\elish\pkg\ElishBrightnessSync.exe" /SC ONSTART /RU "SYSTEM" /RL HIGHEST /F
```
