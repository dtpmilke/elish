# Elish DSDT and Bluetooth PEP configuration

[На русском](../README.ru.md)

The Xiaomi Pad 5 Pro (elish) port uses a modified Qualcomm 8250_MTP DSDT. To reproduce the DSDT from the port source tree, copy the reference 8250/builtin/DSDT_8250_MTP.aml into a working directory as DSDT.aml. From that directory run:

```sh
git apply /path/to/elish/acpi/elish-dsdt.patch
```

SHA-256 before applying the patch: `8c181ad8dd93e37fbc08d2ecc6b7146781375213ba12683d819f18c59feb751a`  
SHA-256 after applying the patch: `2c3c823a010ee6d93fa49b90658bf354d3d01c9b61c47fbb2e29f19327536125`

Experimental boot images used further revisions. [elish-SSDT_BT.asl](elish-SSDT_BT.asl) shows the Bluetooth UART PEP configuration for elish UR18 (QUP0 SE6). For Pad 6 (pipa), check the UART, interrupts and pins against the pipa board description. See the [Bluetooth bring-up notes](../bluetooth/README.md) for the tested driver and firmware combination.
