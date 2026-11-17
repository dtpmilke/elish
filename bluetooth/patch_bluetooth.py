#!/usr/bin/env python3
"""Reproduce the QCA6390 Bluetooth bring-up files from local originals."""
import argparse
import hashlib
import struct
from pathlib import Path

UART_CLOCKS = {7372800, 14745600, 29491200, 32000000, 48000000, 64000000, 80000000, 96000000}
HCI_EDITS = (
    (0x388C, "d3f6ff97", "5df5ff97"),  # Wait for Command Complete on version query
    (0x3A10, "e83f40b9", "e84340b9"),  # Product ID
    (0x3A14, "e9874079", "e98f4079"),  # ROM version
    (0x3A1C, "e8834079", "e88b4079"),  # Patch version
    (0x3A24, "e98b4079", "e94b40b9"),  # 32-bit SoC ID
    (0x3B9C, "0ff6ff97", "99f4ff97"),  # Wait for Command Complete on build info
    (0x1A9E8, "7c9aff97", "0699ff97"),  # Wait for Command Complete on board ID
    (0x149D8, "880640b9c8030036800240b9a801009002a13c9141c33a91b268009418000014",
     "680a40b9a85200b9690e40b9a95600b91b0000141f2003d51f2003d51f2003d5"),  # Save BT_EN connection ID
)


def patch_uart(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    section_count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_header_size = struct.unpack_from("<H", data, pe + 20)[0]
    section_start = pe + 24 + optional_header_size
    table_rva = 0x18410
    for index in range(section_count):
        section = section_start + 40 * index
        virtual_size, rva, raw_size, raw_offset = struct.unpack_from("<4I", data, section + 8)
        if rva <= table_rva < rva + min(virtual_size, raw_size):
            offset = raw_offset + table_rva - rva
            break
    else:
        raise ValueError("UART baud table is outside the file-backed sections")

    # The original table assumes 32x oversampling. The elish QUP uses 16x.
    while True:
        baud, clock, ratio = struct.unpack_from("<3I", data, offset)
        if baud == 0:
            break
        half_clock = clock // 2
        if clock % 2 == 0 and half_clock in UART_CLOCKS:
            clock = half_clock
        elif clock in UART_CLOCKS:
            ratio *= 2
        struct.pack_into("<3I", data, offset, baud, clock, ratio)
        offset += 12


def replace_bytes(data, offset, old_hex, new_hex):
    old = bytes.fromhex(old_hex)
    new = bytes.fromhex(new_hex)
    if len(old) != len(new) or data[offset:offset + len(old)] != old:
        raise ValueError(f"unexpected bytes at file offset {offset:#x}")
    data[offset:offset + len(old)] = new


def patch_hci(data):
    for offset, old, new in HCI_EDITS:
        replace_bytes(data, offset, old, new)

    pe = struct.unpack_from("<I", data, 0x3C)[0]
    checksum_offset = pe + 24 + 64
    struct.pack_into("<I", data, checksum_offset, 0)
    checksum = 0
    for (word,) in struct.iter_unpack("<H", data[:len(data) & ~1]):
        checksum += word
        checksum = (checksum & 0xFFFF) + (checksum >> 16)
    if len(data) & 1:
        checksum += data[-1]
        checksum = (checksum & 0xFFFF) + (checksum >> 16)
    checksum = (checksum & 0xFFFF) + (checksum >> 16)
    struct.pack_into("<I", data, checksum_offset, (checksum + len(data)) & 0xFFFFFFFF)


def patch_nvm(data):
    replace_bytes(data, 0x48, "11", "0e")  # QCA6390 UART baud index, 3.0 Mbit/s
    replace_bytes(data, 0xCD, "01", "00")  # Disable controller deep sleep in the tested NVM


PATCHES = {
    "uart": (patch_uart, "661d2d2a3b072e85538cba2504531787bce60a4b", "9d95b1586a30279f1e8a3ddd8613e09ecc446662"),
    "hci": (patch_hci, "e14122ec0512f1a026d5e41905dafc101f8c05f9", "bb4156cba1bed6b3a80660bca99a6849a35c1329"),
    "nvm": (patch_nvm, "c05e57f8740aa7e0215b533f920f34479b5a425d", "9874090be85afda349a52fe42a2c5ab7646a2b49"),
}


def verify(data, expected, label):
    actual = hashlib.sha1(data).hexdigest()
    if actual != expected:
        raise ValueError(f"{label} SHA-1 is {actual}; expected {expected}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("part", choices=PATCHES)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path, help="new output file")
    args = parser.parse_args()
    try:
        patch, input_hash, output_hash = PATCHES[args.part]
        data = bytearray(args.source.read_bytes())
        verify(data, input_hash, "input")
        patch(data)
        verify(data, output_hash, "output")
        with args.output.open("xb") as output:
            output.write(data)
        print(f"{args.output}: {len(data)} bytes, SHA-1 {output_hash}")
    except (OSError, ValueError) as exc:
        parser.exit(1, f"error: {exc}\n")
