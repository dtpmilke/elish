#!/usr/bin/env python3
"""Build the unsigned ARM64 nvtbb-disp driver using LLVM and a local WDK."""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path, PureWindowsPath

sys.dont_write_bytecode = True
from mkfwz import generate

VERSION = "10.0.28000.0"
KMDF = "1.17"
SOURCE_DIR = Path(__file__).resolve().parent


def build(args):
    wdk = args.wdk.resolve()
    sdk = args.sdk.resolve()
    out = args.out.resolve()
    out.mkdir(parents=True)
    generate(args.firmware_dir, out / "fwz.h")

    wdk_include = wdk / "Include" / VERSION
    includes = [
        out,
        wdk_include / "km/crt",
        wdk_include / "km",
        wdk_include / "shared",
        sdk / "Include" / VERSION / "shared",
        wdk / "Include/wdf/kmdf" / KMDF,
    ]
    flags = [
        "--driver-mode=cl", "--target=aarch64-pc-windows-msvc",
        "/c", "/kernel", "/GS-", "/O2", "/Zl", "/W3", "/Gy",
        "-Wno-ignored-pragma-intrinsic", "-Wno-microsoft-anon-tag",
        "-Wno-pragma-pack", "-Wno-unknown-pragmas",
        "-D_ARM64_", "-D_ARM64", "-DARM64", "-D_WIN64", "-DWINNT=1",
        "-D_WIN32_WINNT=0x0A00", "-DNTDDI_VERSION=0x0A000010",
        "-DKMDF_VERSION_MAJOR=1", "-DKMDF_VERSION_MINOR=17",
        "-DPOOL_NX_OPTIN=1", "-D_KERNEL_MODE",
        "-DNVT_RELEASE", "-DNVT_FIX_D0", "-DNVT_FIX_DISP",
    ]
    objects = []
    for name in ("nvtbb", "stubs"):
        obj = out / f"{name}.obj"
        subprocess.run(
            ["clang", *flags, *(f"/I{path}" for path in includes),
             f"/Fo{obj}", SOURCE_DIR / f"{name}.c"],
            check=True,
        )
        objects.append(obj)

    wdf_libs = wdk / "Lib/wdf/kmdf/ARM64" / KMDF
    archive = wdf_libs / "wdfdriverentry.lib"
    members = subprocess.check_output(["llvm-ar", "t", archive], text=True).splitlines()
    for member in members:
        name = PureWindowsPath(member).name
        original = out / f"raw-{name}"
        obj = out / f"wdf-{name}"
        original.write_bytes(subprocess.check_output(["llvm-ar", "p", archive, member]))
        subprocess.run(
            ["llvm-objcopy", "--remove-section=.impcall", "--remove-section=.chks64", original, obj],
            check=True,
        )
        objects.append(obj)

    kernel_libs = wdk / "Lib" / VERSION / "km/ARM64"
    subprocess.run(
        ["lld-link", "/DRIVER", "/TSAWARE:NO", "/SUBSYSTEM:NATIVE,10.0",
         "/ENTRY:NvtImageEntry", "/MACHINE:ARM64", "/NODEFAULTLIB", "/RELEASE", "/OPT:REF",
         "/SECTION:.data,RW", f"/MAP:{out / 'nvtbb.map'}", f"/OUT:{out / 'nvtbb.sys'}",
         *objects, wdf_libs / "wdfldr.lib", kernel_libs / "ntoskrnl.lib", kernel_libs / "hal.lib"],
        check=True,
    )
    shutil.copyfile(SOURCE_DIR / "nvtbb.inf", out / "nvtbb.inf")
    print(out / "nvtbb.sys")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wdk", required=True, type=Path, help="WDK root containing Include and Lib")
    parser.add_argument("--sdk", required=True, type=Path, help="SDK root containing Include")
    parser.add_argument("--firmware-dir", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path, help="new build directory")
    args = parser.parse_args()
    try:
        build(args)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"error: {exc}\n")
