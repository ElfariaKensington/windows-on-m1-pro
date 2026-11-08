#!/bin/bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
packages=${JW_PACKAGES:-$root/.build/j316s-fresh-input}
out=${JW_OUTPUT:-$here/build}
wdk="$packages/microsoft.windows.wdk.arm64/c"
sdk="$packages/microsoft.windows.sdk.cpp/c"
sdkarm="$packages/microsoft.windows.sdk.cpp.arm64/c"
clang=${CLANG_CL:-/opt/homebrew/opt/llvm/bin/clang-cl}
link=${LLD_LINK:-/opt/homebrew/opt/lld/bin/lld}
mkdir -p "$out"
resource=$("$clang" --print-resource-dir)
"$clang" --target=aarch64-pc-windows-msvc /nologo /c /kernel /O2 /GS /Zl /W4 /WX \
    /D_ARM64_ /DARM64 /D_KERNEL_MODE /DNTDDI_VERSION=0x0A00000E /D_WIN32_WINNT=0x0A00 \
    /DNDIS_MINIPORT_DRIVER /DNDIS630_MINIPORT /DUNICODE /D_UNICODE \
    -Wno-unknown-pragmas -Wno-ignored-attributes -Wno-ignored-pragma-intrinsic \
    /I"$resource/include" \
    /imsvc"$wdk/Include/10.0.28000.0/km" /imsvc"$wdk/Include/10.0.28000.0/km/crt" \
    /imsvc"$wdk/Include/10.0.28000.0/shared" \
    /imsvc"$sdk/Include/10.0.28000.0/shared" /imsvc"$sdk/Include/10.0.28000.0/ucrt" \
    /Fo"$out/driver.obj" -- "$here/driver.c"
"$link" -flavor link /nologo /machine:arm64 /driver /subsystem:native,10.0 /entry:GsDriverEntry \
    /nodefaultlib /dynamicbase /nxcompat /release /filealign:4096 /tsaware:no \
    /integritycheck /merge:.impcall=.rdata "/out:$out/J316Wifi.sys" "$out/driver.obj" \
    "$wdk/Lib/10.0.28000.0/km/ARM64/ndis.lib" \
    "$wdk/Lib/10.0.28000.0/km/ARM64/ntoskrnl.lib" \
    "$wdk/Lib/10.0.28000.0/km/ARM64/hal.lib" \
    "$wdk/Lib/10.0.28000.0/km/ARM64/bufferoverflowfastfailk.lib" \
    "$sdkarm/um/arm64/arm64rt.lib"
cp "$here/J316Wifi.inf" "$here/configure.ps1" "$here/README.md" "$out/"
