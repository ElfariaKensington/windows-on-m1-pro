#!/bin/bash
# Cross-compile AppleAgxRenderAdmission.sys (WDDM 3.2 GpuMmu production profile:
# GpuvaG3Qualification + NativeFrontend + G1b page profile 16) with clang-cl/lld
# against the pinned WDK/SDK 10.0.28000.0.  PLATFORM=j316s|j313 (default j316s).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
platform=${PLATFORM:-j316s}
packages=${AGX_PACKAGES:-$root/.build/j316s-fresh-input}
out=${AGX_OUTPUT:-$root/.build/j316s-agx/kmd-$platform}
wdk="$packages/microsoft.windows.wdk.arm64/c"
sdk="$packages/microsoft.windows.sdk.cpp/c"
sdkarm="$packages/microsoft.windows.sdk.cpp.arm64/c"
v=10.0.28000.0
clang=${CLANG_CL:-/opt/homebrew/opt/llvm/bin/clang-cl}
link=${LLD_LINK:-/opt/homebrew/opt/lld/bin/lld}
mkdir -p "$out/obj"
resource=$("$clang" --print-resource-dir)
defs=(/DDXGKDDI_INTERFACE_VERSION=DXGKDDI_INTERFACE_VERSION_WDDM3_2
      /DADMISSION_GPUVA_G1B_PAGE_PROFILE=16 /DADMISSION_GPUVA_G1B_ALLOCATION_HINT=0
      /DAPPLE_AGX_PLATFORM_EXTERNAL_RENDER_ONLY=1 /DAPPLE_AGX_FULL_CONTEXT0_BROKER=1
      /DAPPLE_AGX_GPUVA_G3_QUALIFICATION=1 /DAPPLE_AGX_EXP907_FRAME_RECEIPT=1
      
      /D_ARM64_ /DARM64 /D_KERNEL_MODE /DNTDDI_VERSION=0x0A000010 /D_WIN32_WINNT=0x0A00
      /DWINNT=1 /DPOOL_NX_OPTIN=1 /DNDEBUG /DAPPLE_AGX_VERSION_BUILD=${AGX_VERSION_BUILD:-2001})
[ "$platform" = j316s ] && defs+=(/DAPPLE_AGX_PLATFORM_J316S=1)
incs=(/Iinclude /I../shared/include /I"$resource/include"
      /imsvc"$wdk/Include/$v/km" /imsvc"$wdk/Include/$v/km/crt" /imsvc"$wdk/Include/$v/shared"
      /imsvc"$sdk/Include/$v/shared" /imsvc"$sdk/Include/$v/um")
flags=(--target=aarch64-pc-windows-msvc /nologo /c /kernel /O2 /GS /Zl /W3 /Gy /Gw
       /std:c11 /GR- /Zc:wchar_t
       -Wno-unknown-pragmas -Wno-ignored-attributes -Wno-ignored-pragma-intrinsic
       -Wno-microsoft-anon-tag -Wno-nonportable-include-path -Wno-pragma-pack
       -Wno-microsoft-enum-forward-reference -Wno-unused-parameter -Wno-unused-function
       -Wno-unused-variable -Wno-unused-const-variable -Wno-missing-braces
       -Wno-microsoft-cast -Wno-invalid-noreturn -Wno-microsoft-include
       -Wno-int-to-void-pointer-cast -Wno-void-pointer-to-int-cast -Wno-builtin-requires-header
       -Wno-incompatible-pointer-types-discards-qualifiers -Wno-sign-compare
       -Werror=implicit-function-declaration -Werror=int-conversion -Werror=incompatible-pointer-types)
cd "$here"
srcs=$(grep -o 'ClCompile Include="[^"]*"' AppleAgxRenderAdmission.vcxproj | sed 's/ClCompile Include="//;s/"$//;s#\\#/#g')
objs=()
fail=0
for s in $srcs; do
  o="$out/obj/$(echo "$s" | tr '/.' '__').obj"
  objs+=("$o")
  if [ -z "${FORCE:-}" ] && [ -f "$o" ] && [ "$o" -nt "$s" ]; then continue; fi
  if ! "$clang" "${flags[@]}" "${defs[@]}" "${incs[@]}" /Fo"$o" -- "$s" 2> "$o.log"; then
    echo "FAIL $s"; grep -m8 "error" "$o.log"; fail=1
  fi
done
"$clang" --target=aarch64-pc-windows-msvc -c src/arm64_hypercalls.S -o "$out/obj/hvc.obj"
objs+=("$out/obj/hvc.obj")
/opt/homebrew/opt/llvm/bin/llvm-rc /nologo /DAPPLE_AGX_VERSION_BUILD=${AGX_VERSION_BUILD:-2001} \
  /I"$sdk/Include/$v/um" /I"$sdk/Include/$v/shared" /FO "$out/obj/version.res" package-version.rc
objs+=("$out/obj/version.res")
[ $fail = 0 ] || { echo "compile failures"; exit 1; }
lib="$wdk/Lib/$v/km/ARM64"
"$link" -flavor link /nologo /machine:arm64 /driver /subsystem:native,10.0 /entry:GsDriverEntry \
  /nodefaultlib /dynamicbase /nxcompat /release /filealign:4096 /tsaware:no /integritycheck /merge:.impcall=.rdata \
  /out:"$out/AppleAgxRenderAdmission.sys" "${objs[@]}" \
  "$lib/displib.lib" "$lib/ntoskrnl.lib" "$lib/hal.lib" "$lib/bufferoverflowfastfailk.lib" \
  "$sdkarm/um/arm64/arm64rt.lib" 2>&1 | tee "$out/link.log"
test -s "$out/AppleAgxRenderAdmission.sys" && echo "built $out/AppleAgxRenderAdmission.sys"
