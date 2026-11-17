# Qualcomm QCA6390 Bluetooth

On November 2, 2026, the radio and Windows Bluetooth stack came up on the elish test tablet. The working setup used the 16× UART patch, an HCI driver adapted for QCA6390, and a matching 3 Mbit/s NVM. These commands reproduce the three local reference files from the original inputs.

```sh
python bluetooth/patch_bluetooth.py uart /path/to/qcuart8250.sys qcuart8250.patched.sys
python bluetooth/patch_bluetooth.py hci /path/to/qcbtfmuart8250.sys qcbtfmuart8250.patched.sys
python bluetooth/patch_bluetooth.py nvm /path/to/htnv20.bin crnv20.bin
```

| Component | Input SHA-1 | Output SHA-1 |
| --- | --- | --- |
| UART | `661d2d2a3b072e85538cba2504531787bce60a4b` | `9d95b1586a30279f1e8a3ddd8613e09ecc446662` |
| HCI | `e14122ec0512f1a026d5e41905dafc101f8c05f9` | `bb4156cba1bed6b3a80660bca99a6849a35c1329` |
| NVM | `c05e57f8740aa7e0215b533f920f34479b5a425d` | `9874090be85afda349a52fe42a2c5ab7646a2b49` |

The tested firmware was `htbtfw20.tlv` from elish, used as `crbtfw20.tlv` with SHA-1 `88e1a04c9d8e5402e8539b92ed8a157aca2e4092`. The tested Windows setup used `BaudRate=3000000`, `SWInBandSleepEnabled=1`, `ExplicitRtsWakeup=0`, `IsRome=1`, and `DoNotEnterD3=1`, with the elish UR18 and Bluetooth power setup in ACPI.

The driver edits invalidate the original signatures. Installation requires a matching INF/CAT package, signed patched drivers, and firmware from a local copy. The current image needs an end-to-end Bluetooth check. Suspend and resume with the radio enabled are still under investigation.
