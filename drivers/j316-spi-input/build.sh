#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
packages=${JI_PACKAGES:-$root/.build/j316s-fresh-input}
out=${JI_OUTPUT:-$packages/build}
wdk="$packages/microsoft.windows.wdk.arm64/c"
sdk="$packages/microsoft.windows.sdk.cpp/c"
sdkarm="$packages/microsoft.windows.sdk.cpp.arm64/c"
clang=${CLANG_CL:-/opt/homebrew/opt/llvm/bin/clang-cl}
link=${LLD_LINK:-/opt/homebrew/opt/lld/bin/lld}
mkdir -p "$out"
resource=$("$clang" --print-resource-dir)
for source in driver protocol; do
    "$clang" --target=aarch64-pc-windows-msvc /nologo /c /O2 /GS /Zl /W4 /WX \
        /D_ARM64_ /DARM64 /D_KERNEL_MODE /DNTDDI_VERSION=0x0A00000E /D_WIN32_WINNT=0x0A00 \
        /DKMDF_VERSION_MAJOR=1 /DKMDF_VERSION_MINOR=33 /DUNICODE /D_UNICODE \
        -Wno-unknown-pragmas -Wno-ignored-attributes -Wno-ignored-pragma-intrinsic \
        /I"$resource/include" \
        /imsvc"$wdk/Include/10.0.28000.0/km" /imsvc"$wdk/Include/10.0.28000.0/km/crt" \
        /imsvc"$wdk/Include/10.0.28000.0/shared" \
        /imsvc"$wdk/Include/wdf/kmdf/1.33" /imsvc"$sdk/Include/10.0.28000.0/shared" \
        /imsvc"$sdk/Include/10.0.28000.0/ucrt" \
        /Fo"$out/$source.obj" -- "$root/drivers/j316-spi-input/$source.c"
done
"$link" -flavor link /nologo /machine:arm64 /driver /subsystem:native,10.0 /entry:FxDriverEntry \
    /nodefaultlib /dynamicbase /nxcompat /release /filealign:4096 /tsaware:no \
    /integritycheck /merge:.impcall=.rdata "/out:$out/J316SpiInput.sys" \
    "$out/driver.obj" "$out/protocol.obj" \
    "$wdk/Lib/wdf/kmdf/ARM64/1.33/wdfdriverentry.lib" \
    "$wdk/Lib/wdf/kmdf/ARM64/1.33/wdfldr.lib" \
    "$wdk/Lib/10.0.28000.0/km/ARM64/ntoskrnl.lib" \
    "$wdk/Lib/10.0.28000.0/km/ARM64/hal.lib" \
    "$wdk/Lib/10.0.28000.0/km/ARM64/vhfkm.lib" \
    "$wdk/Lib/10.0.28000.0/km/ARM64/bufferoverflowfastfailk.lib" \
    "$sdkarm/um/arm64/arm64rt.lib"
cp "$root/drivers/j316-spi-input/J316SpiInput.inf" "$out/"
if [[ -n "${JI_CERTIFICATE:-}" ]]; then
    python3 - "$JI_CERTIFICATE" "$out/certificate.h" <<'PY'
from pathlib import Path
import sys
data = Path(sys.argv[1]).read_bytes()
Path(sys.argv[2]).write_text("static const unsigned char ji_certificate[] = {" +
                            ",".join(map(str, data)) + "};\n")
PY
    "$clang" --target=aarch64-pc-windows-msvc /nologo /c /O2 /GS- /Zl /W4 /WX \
        /DUNICODE /D_UNICODE /D_ARM64_ /D_WIN32_WINNT=0x0A00 /I"$out" \
        /imsvc"$sdk/Include/10.0.28000.0/um" /imsvc"$sdk/Include/10.0.28000.0/shared" \
        /imsvc"$wdk/Include/10.0.28000.0/km/crt" \
        /Fo"$out/tools.obj" -- "$root/drivers/j316-spi-input/tools.c"
    "$link" -flavor link /nologo /machine:arm64 /subsystem:console /entry:mainCRTStartup \
        /nodefaultlib "/out:$out/J316Tools.exe" "$out/tools.obj" \
        "$sdkarm/um/arm64/kernel32.Lib" "$sdkarm/um/arm64/crypt32.Lib" \
        "$sdkarm/um/arm64/user32.Lib"
fi
