# Build and validation

The table lists what you can reproduce from a local copy and where to find instructions.

| Component | Local inputs | Result and validation |
| --- | --- | --- |
| [UEFI](../README.md#uefi-chip-info) | Mu-Silicium checkout | Apply the SOID (`0001`) and PCIe BDF-to-SID (`0002`) patches, then build using the upstream instructions. On the tested elish build, Windows reported `SOID = 0x164` and cold boot Wi-Fi initialization achieved 8/8 FIRST_TRY_OK passes. |
| [Wi-Fi](../README.md#wi-fi) | Original `qcwlan6800sc81.sys`, version `1.0.1760.20` | Reproduce the hash-checked unsigned driver. Combined with the UEFI `0002` BDF-to-SID patch, Wi-Fi initializes on the first attempt without firmware-loading retries. |
| [K81 display](../README.md#k81-display-panel) | Original K81 UEFI panel XML | Generate the hash-checked XML used for internal panel output. Long-running GPU stability remains open. |
| [Touchscreen](../touch/README.md) | Windows SDK, ARM64 WDK, LLVM, and two NT36523 firmware files | Build an unsigned ARM64 KMDF driver. The earlier signed driver accepted touch input. The new build needs a tablet test. |
| [Bluetooth](../bluetooth/README.md) | Original Qualcomm UART and HCI drivers, and elish QCA6390 firmware and NVM | Reproduce the three hash-checked files from the bring-up. The radio and Windows stack came up on the earlier image. The current image needs a regression run. |

Use the commands in each component's documentation from the repository root unless the instructions specify a separate upstream checkout. The hash checks cover generated files. Windows installation additionally needs driver signing, device provisioning, and a package with the matching firmware and hardware configuration.
