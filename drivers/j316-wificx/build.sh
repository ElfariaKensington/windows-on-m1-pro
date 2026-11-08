#!/bin/bash
# Cross-compile J316WifiCx.sys (KMDF 1.33 + NetAdapterCx 2.2 + WiFiCx 1.0, ARM64) with
# clang-cl/lld against the pinned WDK/SDK 10.0.28000.0 used by the sibling drivers.
# No catalog, signing, installation or hardware access.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
packages=${JX_PACKAGES:-$root/.build/j316s-fresh-input}
out=${JX_OUTPUT:-$here/build}
wdk="$packages/microsoft.windows.wdk.arm64/c"
sdk="$packages/microsoft.windows.sdk.cpp/c"
sdkarm="$packages/microsoft.windows.sdk.cpp.arm64/c"
v=10.0.28000.0
clang=${CLANG_CL:-/opt/homebrew/opt/llvm/bin/clang-cl}
link=${LLD_LINK:-/opt/homebrew/opt/lld/bin/lld}
mkdir -p "$out" "$out/overlay"
# clang rejects WDF's C++ forward declaration "enum _WDF_REQUEST_TYPE : int;" followed by a
# definition without a fixed type (MSVC accepts it). Use a build-local copy without ": int".
sed 's/^enum _WDF_REQUEST_TYPE : int;/enum _WDF_REQUEST_TYPE;/' \
    "$wdk/Include/wdf/kmdf/1.33/wdfdevice.h" > "$out/overlay/wdfdevice.h"
cp "$wdk/Include/wdf/kmdf/1.33/wdf.h" "$out/overlay/wdf.h"   # its "wdfdevice.h" resolves beside it
resource=$("$clang" --print-resource-dir)
"$clang" --target=aarch64-pc-windows-msvc /nologo /c /kernel /O2 /GS /Zl /W4 /WX /std:c++17 \
    /GR- /EHs-c- /Zc:threadSafeInit- \
    /D_ARM64_ /DARM64 /D_KERNEL_MODE /DNTDDI_VERSION=0x0A00000E /D_WIN32_WINNT=0x0A00 \
    /DKMDF_VERSION_MAJOR=1 /DKMDF_VERSION_MINOR=33 \
    /DNETADAPTER_VERSION_MAJOR=2 /DNETADAPTER_VERSION_MINOR=2 \
    /DWIFI_VERSION_MAJOR=1 /DWIFI_VERSION_MINOR=0 \
    /DUNICODE /D_UNICODE \
    -Wno-unknown-pragmas -Wno-ignored-attributes -Wno-ignored-pragma-intrinsic \
    -Wno-microsoft-anon-tag -Wno-nonportable-include-path -Wno-pragma-pack \
    -Wno-microsoft-enum-forward-reference -Wno-unused-parameter \
    -Wno-microsoft-exception-spec -Wno-duplicate-decl-specifier \
    /imsvc"$out/overlay" /I"$resource/include" \
    /imsvc"$wdk/Include/$v/km" /imsvc"$wdk/Include/$v/km/crt" /imsvc"$wdk/Include/$v/shared" \
    /imsvc"$wdk/Include/wdf/kmdf/1.33" \
    /imsvc"$wdk/Include/$v/km/netcx/kmdf/adapter/2.2" \
    /imsvc"$sdk/Include/$v/shared/netcx/shared/1.0" \
    /imsvc"$wdk/Include/$v/km/wificx/1.0" \
    /imsvc"$wdk/Include/$v/km/wlan/2.0" \
    /imsvc"$sdk/Include/$v/shared" /imsvc"$sdk/Include/$v/ucrt" \
    /Fo"$out/driver.obj" -- "$here/driver.cpp"
lib="$wdk/Lib/$v/km/ARM64"
"$link" -flavor link /nologo /machine:arm64 /driver /subsystem:native,10.0 /entry:FxDriverEntry \
    /nodefaultlib /dynamicbase /nxcompat /release /filealign:4096 /tsaware:no \
    /integritycheck /merge:.impcall=.rdata "/out:$out/J316WifiCx.sys" "$out/driver.obj" \
    "$wdk/Lib/wdf/kmdf/ARM64/1.33/WdfDriverEntry.lib" "$wdk/Lib/wdf/kmdf/ARM64/1.33/WdfLdr.lib" \
    "$lib/netcx/kmdf/adapter/2.2/netadaptercxstub.lib" \
    "$lib/wificx/1.0/wificxstub.lib" \
    "$lib/wlan/2.0/WificxTLVGenParse.lib" \
    "$lib/ntoskrnl.lib" "$lib/hal.lib" "$lib/ndis.lib" "$lib/wmilib.lib" \
    "$lib/BufferOverflowFastFailK.lib" \
    "$sdkarm/um/arm64/arm64rt.lib"
cp "$here/J316WifiCx.inf" "$here/README.md" "$here/switch-mode.ps1" "$out/" 2>/dev/null || true
echo "built $out/J316WifiCx.sys"
