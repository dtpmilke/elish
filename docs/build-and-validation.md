# Build and validation

The table lists what you can reproduce from a local copy and where to find instructions.

| Component | Local inputs | Result and validation |
| --- | --- | --- |
| [UEFI](../README.md#uefi-chip-info) | Mu-Silicium checkout | Apply the SOID source patch and build using the upstream instructions for your platform. On the tested elish build, the live Windows DSDT reported `SOID = 0x164`. |
| [Wi-Fi](../README.md#wi-fi) | Original `qcwlan6800sc81.sys`, version `1.0.1760.20` | Reproduce the hash-checked unsigned driver. The earlier setup connected to Wi-Fi and sometimes needed firmware-loading retries. |
| [K81 display](../README.md#k81-display-panel) | Original K81 UEFI panel XML | Generate the hash-checked XML used for internal panel output. Long-running GPU stability remains open. |
| [Touchscreen](../touch/README.md) | Windows SDK, ARM64 WDK, LLVM, and two NT36523 firmware files | Build an unsigned ARM64 KMDF driver. The earlier signed driver accepted touch input. The new build needs a tablet test. |
| [Bluetooth](../bluetooth/README.md) | Original Qualcomm UART and HCI drivers, and elish QCA6390 firmware and NVM | Reproduce the three hash-checked files from the bring-up. The radio and Windows stack came up on the earlier image. The current image needs a regression run. |

Use the commands in each component's documentation from the repository root unless the instructions specify a separate upstream checkout. The hash checks cover generated files. Windows installation additionally needs driver signing, device provisioning, and a package with the matching firmware and hardware configuration.
