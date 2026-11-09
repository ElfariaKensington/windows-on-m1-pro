#!/bin/bash
# Cross-compile AppleAgxRenderAdmissionUmd.dll (ARM64, native frontend + GPUVA winsys,
# matching the J313 production profile) with clang-cl/lld-link against the MSVC CRT
# (xwin) and the pinned 10.0.28000 SDK/WDK, linking the native Mesa runtime archive.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
platform=${PLATFORM:-j316s}
T=${AGX_TOOLCHAINS:-/Users/elfaria/win11/toolchains}
runtime=${NATIVE_RUNTIME:-$T/native-runtime-arm64-mt2}
native=${NATIVE_SOURCE:-$T/native-gpuva-arm64}
mesa=$T/mesa; gen=$T/mesa-host; nir=$T/nir-arm64
out=${AGX_OUTPUT:-$root/.build/j316s-agx/umd-$platform}
mkdir -p "$out/obj"
export PATH=$T/hosttools:/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:$PATH
X=$T/msvc-sysroot
cc=$T/hosttools/clang-cl
base=(--target=aarch64-pc-windows-msvc /nologo /c /O2 /Z7 /MT /GS /Gy /Gw /utf-8 /EHsc /W3
      /imsvc"$X/crt/include" /imsvc"$X/sdk/include/ucrt" /imsvc"$X/sdk/include/um" /imsvc"$X/sdk/include/shared"
      /FI"$T/overlay/uatomic_clang_compat.h"
      -Wno-unused-command-line-argument -Wno-unknown-pragmas -Wno-microsoft-anon-tag
      -Wno-ignored-attributes -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable
      -Wno-missing-field-initializers -Wno-sign-compare -Wno-nonportable-include-path
      -Wno-error=incompatible-pointer-types -Wno-error=int-conversion
      -Wno-error=incompatible-function-pointer-types -Wno-c++11-narrowing
      /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /DWIN32 /D_WINDOWS /D_USRDLL
      /DAPPLE_AGX_UMD_ADMISSION_TRACE=1 /DAPPLE_AGX_UMD_NATIVE_FRONTEND=1
      /DAPPLE_AGX_GPUVA_WINSYS=1 /DAPPLE_AGX_EXP907_FRAME_RECEIPT=1
      /DADMISSION_UMD_PIPE_FACTORY_TEST=1 /DAPPLE_AGX_D3D12_NATIVE_OWNER=1
      /DHAVE_FUNC_ATTRIBUTE_PACKED=1 /DAPPLE_AGX_VERSION_BUILD=${AGX_VERSION_BUILD:-2001}
      /Daccess=_access /Dunlink=_unlink /Dstrdup=_strdup /Dstricmp=_stricmp)
[ "$platform" = j316s ] && base+=(/DAPPLE_AGX_PLATFORM_J316S=1)
inc=(/Isrc /Iinclude /I../include /I../../shared/include /I../../mesa/winsys
     /I../../mesa/dxbc /I../../mesa/d3d11
     /I"$native/include" /I"$native/src" /I"$native/src/asahi" /I"$native/src/asahi/lib"
     /I"$native/src/asahi/layout" /I"$native/src/asahi/compiler" /I"$native/src/asahi/libagx"
     /I"$native/src/gallium/drivers/asahi" /I"$native/src/gallium/frontends/d3d10umd"
     /I"$T/overlay"
     /I"$nir/src" /I"$nir/include" /I"$nir/src/compiler" /I"$nir/src/compiler/nir"
     /I"$mesa/include" /I"$mesa/src" /I"$mesa/include/winddk" /I"$mesa/src/gallium/include"
     /I"$mesa/src/gallium/auxiliary" /I"$mesa/src/compiler" /I"$mesa/src/compiler/nir"
     /I"$gen/src" /I"$gen/src/asahi/compiler" /I"$gen/src/asahi/libagx" /I"$gen/src/asahi/genxml"
     /I"$gen/src/asahi/lib" /I"$mesa/src/asahi/isa" /I"$mesa/src/poly" /I"$mesa/src/poly/nir")
cd "$here"
# file|mode (c, cpp=CompileAsCpp, cpp17)
units="src/umd.c|c ../../d3d12/agx_d3d12_ddi.cpp|cpp17 src/umd_runtime_device.c|cpp
src/umd_win32_screen.c|cpp src/umd_resource_lifetime.c|c src/umd_draw_composer.c|cpp
../src/render_win32_transport.c|c src/direct_flip_contract.c|c ../src/render_allocation.c|c
../../shared/src/apple_agx_win32_abi.c|c ../../mesa/winsys/agx_win32_transport.c|c
../../mesa/winsys/agx_win32_screen.c|c ../../mesa/winsys/agx_d3d10_windows.cpp|cpp17
../../mesa/winsys/agx_d3d12_windows.cpp|cpp17 src/umd_asahi_batch_adapter.c|cpp
src/umd_asahi_owner.c|cpp ../../mesa/winsys/agx_win32_native_bo.c|c
../../mesa/winsys/agx_win32_construction_address.c|c ../../mesa/winsys/agx_win32_native_device.c|c
../../mesa/winsys/agx_win32_reloc_capture.c|c src/umd_gpuva_windows.c|cpp
../../mesa/winsys/agx_win32_gpuva.c|c"
objs=(); fail=0
for u in $units; do
  f=${u%|*}; m=${u#*|}
  o="$out/obj/$(echo "$f" | tr '/.' '__').obj"; objs+=("$o")
  case $m in c) mode=(/TC /clang:-std=c11);; cpp) mode=(/TP /std:c++17);; cpp17) mode=(/TP /std:c++17);; esac
  if ! "$cc" "${base[@]}" "${mode[@]}" "${inc[@]}" /Fo"$o" "$f" > "$o.log" 2>&1; then
    echo "FAIL $f"; grep -m6 "error" "$o.log"; fail=1
  fi
done
[ $fail = 0 ] || exit 1
llvm-rc /nologo /DAPPLE_AGX_VERSION_BUILD=${AGX_VERSION_BUILD:-2001} /DAPPLE_AGX_PACKAGE_DLL=1 \
  /I"$root/.build/j316s-fresh-input/microsoft.windows.sdk.cpp/c/Include/10.0.28000.0/um" \
  /I"$root/.build/j316s-fresh-input/microsoft.windows.sdk.cpp/c/Include/10.0.28000.0/shared" \
  /FO "$out/obj/version.res" ../package-version.rc
objs+=("$out/obj/version.res")
lld-link /nologo /dll /machine:arm64 /nodefaultlib:msvcrt.lib /nodefaultlib:msvcprt.lib /out:"$out/AppleAgxRenderAdmissionUmd.dll" \
  /def:AppleAgxRenderAdmissionUmd.def /opt:ref /opt:icf /debug /pdb:"$out/AppleAgxRenderAdmissionUmd.pdb" \
  /map:"$out/AppleAgxRenderAdmissionUmd.map" \
  /libpath:"$X/crt/lib/aarch64" /libpath:"$X/sdk/lib/ucrt/aarch64" /libpath:"$X/sdk/lib/um/aarch64" \
  "${objs[@]}" "$runtime/native_runtime.lib" "$nir/src/compiler/nir/libnir.a" "$nir/src/compiler/libcompiler.a" \
  "$nir/src/util/libmesa_util.a" "$nir/src/c11/impl/libmesa_util_c11.a" "$nir/src/util/blake3/libblake3.a" \
  libcmt.lib libucrt.lib libvcruntime.lib libcpmt.lib legacy_stdio_definitions.lib onecoreuap.lib \
  synchronization.lib advapi32.lib user32.lib kernel32.lib gdi32.lib 2>&1 | tee "$out/link.log" | grep -v "^lld-link: warning: .*duplicate" | head -40
test -s "$out/AppleAgxRenderAdmissionUmd.dll" && echo "built $out/AppleAgxRenderAdmissionUmd.dll"
