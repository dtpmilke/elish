#!/bin/bash
# Build elishotg.sys (ARM64 WDM one-shot driver) with clang-cl + lld-link.
# Set WDK_ROOT, SDK_ROOT, CERT_DIR (optional, for test-signing) before running.
set -e
cd "$(dirname "$0")"
W="${WDK_ROOT:-$HOME/winsdk/microsoft.windows.wdk.arm64.10.0.28000.2526/c}"
S="${SDK_ROOT:-$HOME/winsdk/microsoft.windows.sdk.cpp.10.0.28000.2957/c}"
V="${WINSDK_VER:-10.0.28000.0}"
OUT="${OUT:-./out}"
mkdir -p "$OUT"

clang --driver-mode=cl --target=aarch64-pc-windows-msvc /c /kernel /GS- /O2 /Zl /W3 /Gy \
  -Wno-ignored-pragma-intrinsic -Wno-microsoft-anon-tag -Wno-pragma-pack -Wno-unknown-pragmas \
  -D_ARM64_ -D_ARM64 -DARM64 -D_WIN64 -DWINNT=1 -D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000010 \
  -DPOOL_NX_OPTIN=1 -D_KERNEL_MODE \
  "/I$W/Include/$V/km/crt" "/I$W/Include/$V/km" "/I$W/Include/$V/shared" "/I$S/Include/$V/shared" \
  "/Fo$OUT/" elishotg.c

lld-link /DRIVER /SUBSYSTEM:NATIVE,10.0 /TSAWARE:NO /ENTRY:DriverEntry /MACHINE:ARM64 /NODEFAULTLIB /RELEASE /OPT:REF \
  "/OUT:$OUT/elishotg.unsigned.sys" "$OUT/elishotg.obj" \
  "$W/Lib/$V/km/ARM64/ntoskrnl.lib" "$W/Lib/$V/km/ARM64/hal.lib"

if [ -n "$CERT_DIR" ] && [ -f "$CERT_DIR/chain.pem" ] && [ -f "$CERT_DIR/leaf.key" ]; then
  osslsigncode sign -certs "$CERT_DIR/chain.pem" -key "$CERT_DIR/leaf.key" -h sha256 -n elishotg \
    -in "$OUT/elishotg.unsigned.sys" -out "$OUT/elishotg.sys"
  osslsigncode verify -CAfile "$CERT_DIR/chain.pem" "$OUT/elishotg.sys"
fi
ls -la "$OUT"
