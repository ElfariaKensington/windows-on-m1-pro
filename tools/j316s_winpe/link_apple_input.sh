#!/bin/sh
set -eu

if [ "$#" -ne 5 ]; then
    echo "Usage: $0 WDK_C_DIR ARM64_SDK_LIB_DIR OBJECT_RESPONSE_FILE LLD_LINK OUTPUT_SYS" >&2
    exit 2
fi

wdk=$1
sdk_lib=$2
objects=$3
linker=$4
output=$5
kernel_lib="$wdk/Lib/10.0.28000.0/km/ARM64"
wdf_lib="$wdk/Lib/wdf/kmdf/ARM64/1.33"

# This layout passed WinPE's ARM64 kernel image load and entered KMDF.
"$linker" /machine:arm64 /driver /subsystem:native,10.0 /entry:FxDriverEntry \
    /nodefaultlib /dynamicbase /nxcompat /release /filealign:4096 /tsaware:no \
    /integritycheck /merge:.impcall=.rdata "/out:$output" "@$objects" \
    "$wdf_lib/wdfdriverentry.lib" "$wdf_lib/wdfldr.lib" \
    "$kernel_lib/ntoskrnl.lib" "$kernel_lib/hal.lib" "$kernel_lib/vhfkm.lib" \
    "$kernel_lib/cng.lib" "$kernel_lib/bufferoverflowfastfailk.lib" "$sdk_lib/arm64rt.lib"
