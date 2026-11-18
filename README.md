# Xiaomi Pad 5 Pro (elish) fixes

[На русском](README.ru.md)

These source files cover verified pieces of the Windows on ARM port. The Python tools use only the standard library. Supply the original input files from your own copy. The patchers verify their input and output hashes and require a new output path.

The UEFI port builds on [Project Silicium's Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium). This repository contains elish patches and driver sources. Start with the [build and validation guide](docs/build-and-validation.md). See [contributing](CONTRIBUTING.md) for the UEFI fork and upstream PR workflow.

## UEFI chip info

[The patch](uefi/0001-fix-chipinfo-output-width.patch) fixes the output buffers passed to `GetChipFamily` and `GetModemSupport`. The protocol writes 32-bit values. Passing pointers to the 16-bit ACPI fields could overwrite adjacent stack data, including `SOID`. On the tablet, the tested build reported `SOID = 0x164` in the live DSDT.

Apply from the root of a [Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) checkout. The tested baseline used commit `4bd8ae83bf37de41777f19e0777db70f59383a6d`.

```sh
git apply --check /path/to/elish/uefi/0001-fix-chipinfo-output-width.patch
git apply /path/to/elish/uefi/0001-fix-chipinfo-output-width.patch
```

## Wi-Fi

[The patcher](wifi/patch_qcwlan.py) accepts the original `qcwlan6800sc81.sys` from package version `1.0.1760.20`. It applies the two verified byte edits, removes the PE certificate table, and updates the checksum. The unsigned output matches the known reference byte for byte. Wi-Fi worked on the test tablet, with occasional firmware-loading retries.

```sh
python wifi/patch_qcwlan.py --orig /path/to/original/qcwlan6800sc81.sys --out qcwlan6800sc81.unsigned.sys
```

| File | SHA-1 |
| --- | --- |
| Original | `6f1f2f18eaebf69be6da7717ea1fbd7e482b0ce3` |
| Unsigned output | `a89170aa3c6b81816caee4f280735e40dc3063f8` |

Installation requires signing the patched driver and using a matching driver package.

## K81 display panel

[The generator](display/gen_panel_xml.py) adds panel identification, synthetic EDID fields, and physical dimensions to the K81 configuration. It carries the timings and DSI sequences through from the UEFI input. The internal panel displayed an image in the E1cp test with this configuration. GPU stability remains under investigation.

```sh
python display/gen_panel_xml.py /path/to/Panel_K81_42_02_0a_dualdsi_cphy_lcd_vid.xml elish_k81_42_02_0a.xml
```

| File | SHA-256 |
| --- | --- |
| Original XML, with LF line endings | `62aa7bd9b434c7622da7b3bb1709c69c1962a9a3ea026c2fea25602b557f480f` |
| Generated configuration | `0337bebc6fbf8359181e5b7fc33a4076064105f8d86b532145bd7ab241ee1641` |

## Touchscreen

[nvtbb-disp](touch/README.md) drives the Novatek NT36523 touchscreen over GPIO SPI. Touch input was confirmed on the tablet. The build generates its firmware header from the two local Android firmware files.

## Bluetooth

[The QCA6390 patcher](bluetooth/README.md) reproduces the UART driver, Bluetooth HCI driver, and NVM used when the radio and Windows Bluetooth stack came up on November 2, 2026. The current Windows image still needs a Bluetooth regression run.
