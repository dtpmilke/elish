# Novatek NT36523 touchscreen

`nvtbb-disp` is a KMDF HID driver for the Xiaomi Pad 5 Pro. It uses GPIO 8–11 for SPI. Touch input was confirmed on the earlier signed build. The new native Windows build was verified offline. Rotation, edge accuracy, and watchdog reloads during idle still need testing.

## Build on Windows

Put Python 3, `clang`, `lld-link`, `llvm-ar`, and `llvm-objcopy` on PATH. Supply local ARM64 WDK and Windows SDK roots with version `10.0.28000.0` headers, and KMDF `1.17` headers and libraries from the WDK. The tested package versions were WDK `10.0.28000.2526` and SDK `10.0.28000.2957`.

Use a directory containing these files from your own elish Android `/vendor/firmware`.

| Firmware | SHA-256 | Panel value |
| --- | --- | --- |
| `novatek_nt36523_k81a_fw01.bin` | `33d2e0a9217df7e898a96728631f3b6decd85ddb7d4112b38b9d7d6d162d6938` | `1` for panel ID `0x42` |
| `novatek_nt36523_k81a_fw02.bin` | `f38633bf75ca75d735785fe3e802f9b89fb4a25aaeb8783b450c24cb333cfcf1` | `2` for panel IDs `0x08` and `0x35` |

```powershell
python touch/build.py --wdk "C:\path\to\wdk\c" --sdk "C:\path\to\sdk\c" --firmware-dir "C:\path\to\firmware" --out touch/out
```

The build produces `touch/out/nvtbb.sys` and copies the bring-up INF. The SYS is unsigned and embeds the supplied firmware. The INF describes the `ROOT\NVTBB` device and `mshidkmdf` filter from the earlier setup. The root device must be provisioned and the driver package signed before installation.

The firmware download sequence follows the NT36523 Linux driver from Xiaomi's `elish-r-oss` source. Its firmware update source credits Novatek, Inc. (2010–2018) and Xiaomi, Inc. (2021) and carries a GPL-2.0-or-later notice. [Upstream NT36523 driver](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/elish-r-oss/drivers/input/touchscreen/nt36523).
