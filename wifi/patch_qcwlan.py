#!/usr/bin/env python3
"""Reproduce the unsigned elish Wi-Fi patch for qcwlan6800sc81.sys."""
import argparse
import hashlib
import struct
from pathlib import Path

ORIG_SHA1 = "6f1f2f18eaebf69be6da7717ea1fbd7e482b0ce3"
OUT_SHA1 = "a89170aa3c6b81816caee4f280735e40dc3063f8"
EDITS = [(0x1417F0, 0xA5, 0x1C), (0x1417F3, 0x35, 0x14)]
SECDIR_FILE_OFFSET = 0x2ADE00
SECDIR_SIZE = 0x5518


def sha1(b: bytes) -> str:
    return hashlib.sha1(b).hexdigest()


def die(message: str):
    raise SystemExit(f"error: {message}")


def pe_checksum(data: bytes, csum_off: int) -> int:
    buf = bytearray(data)
    buf[csum_off:csum_off + 4] = b"\0\0\0\0"
    s = 0
    n = len(buf)
    for i in range(0, n - 1, 2):
        s += buf[i] | (buf[i + 1] << 8)
        if s > 0xFFFFFFFF:
            s = (s & 0xFFFF) + (s >> 16)
    if n & 1:
        s += buf[-1]
    s = (s & 0xFFFF) + (s >> 16)
    s = (s & 0xFFFF) + (s >> 16)
    return (s + n) & 0xFFFFFFFF


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--orig", type=Path, required=True, help="original WHQL driver")
    ap.add_argument("--out", type=Path, required=True, help="new unsigned output file")
    ap.add_argument("--ref", type=Path, help="optional reference for a byte-for-byte comparison")
    args = ap.parse_args()

    data = bytearray(args.orig.read_bytes())
    h = sha1(data)
    if h != ORIG_SHA1:
        die(f"input SHA-1 is {h}; expected {ORIG_SHA1}")

    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_off:pe_off + 4] != b"PE\0\0":
        die("missing PE signature")
    if struct.unpack_from("<H", data, pe_off + 0x18)[0] != 0x20B:
        die("expected a PE32+ image")
    csum_off = pe_off + 0x58
    sec_off = pe_off + 0xA8
    certificate_offset, certificate_size = struct.unpack_from("<II", data, sec_off)
    if (certificate_offset, certificate_size) != (SECDIR_FILE_OFFSET, SECDIR_SIZE):
        die("unexpected certificate table offset or size")
    if certificate_offset + certificate_size != len(data):
        die("expected the certificate table at the end of the file")

    data[sec_off:sec_off + 8] = b"\0" * 8
    del data[certificate_offset:]

    for off, old, new in EDITS:
        if data[off] != old:
            die(f"byte at {off:#x} is {data[off]:#04x}; expected {old:#04x}")
        data[off] = new

    csum = pe_checksum(bytes(data), csum_off)
    struct.pack_into("<I", data, csum_off, csum)

    out = bytes(data)
    h = sha1(out)
    if h != OUT_SHA1:
        die(f"output SHA-1 is {h}; expected {OUT_SHA1}")
    if args.ref and args.ref.read_bytes() != out:
        die("output differs from the reference file")
    with args.out.open("xb") as output:
        output.write(out)
    print(f"{args.out}: {len(out)} bytes, SHA-1 {h}, PE checksum {csum:#010x}")


if __name__ == "__main__":
    try:
        main()
    except OSError as exc:
        die(str(exc))
