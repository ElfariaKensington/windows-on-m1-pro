#!/bin/bash
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
out="$root/.build/j316s-wdk"
wim_source="$out/boot-j316s-interactive.wim"
media_source="$out/setup-media-j316s-interactive.img"
candidate="${J316S_DIAG_CANDIDATE:-$root/dist/J316s/J316s-Setup-interactive-input-diag-disk-v3.bin}"
manifest="${J316S_DIAG_MANIFEST:-$root/dist/J316s/J316s-Setup-interactive-input-diag-disk-v3.manifest.json}"
rollback="$root/.build/j316s-esp-usb-input/j316s-interactive-input.img"
diagnostics_disk="$root/.build/j316s-diagnostics/diagnostics-disk-v2.img"
sdk_lib="$out/microsoft.windows.sdk.cpp.arm64/c/um/arm64"
clang=${CLANG_CL:-/opt/homebrew/opt/llvm/bin/clang-cl}
lld=${LLD_LINK:-/opt/homebrew/opt/lld/bin/lld}

for required in "$wim_source" "$media_source" "$rollback" "$diagnostics_disk" "$clang" "$lld" \
    "$sdk_lib/kernel32.Lib" "$sdk_lib/SetupAPI.Lib" \
    "$root/m1n1_windows/build/m1n1-raw.elf"; do
    if [[ ! -f "$required" ]]; then
        echo "Required build input is missing: $required" >&2
        exit 1
    fi
done
if [[ -e "$candidate" || -e "$manifest" ]]; then
    echo "Refusing to overwrite an existing diagnostics candidate" >&2
    exit 1
fi

scratch=$(mktemp -d "$root/.build/j316s-input-diag.XXXXXX")
trap 'rm -rf "$scratch"' EXIT

"$clang" --target=arm64-pc-windows-msvc /nologo /c /O2 /Zl /GS- -fno-builtin \
    /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 \
    /I"$root/tools/j316s_winpe/include" \
    /I"$root/drivers/apple-input/windows/include" \
    /Fo"$scratch/AppleInputLiveDiag.obj" \
    -- "$root/tools/j316s_winpe/AppleInputLiveDiag.c"

"$lld" -flavor link /nologo /machine:arm64 /subsystem:console \
    /entry:mainCRTStartup /nodefaultlib /dynamicbase /nxcompat \
    "/out:$scratch/AppleInputLiveDiag.exe" \
    "$scratch/AppleInputLiveDiag.obj" \
    "$sdk_lib/kernel32.Lib" "$sdk_lib/SetupAPI.Lib"

python3 - "$root/tools/j316s_winpe" "$scratch" <<'PY'
from pathlib import Path
import sys

source, output = map(Path, sys.argv[1:])
for name in ("start_setup.cmd", "persist_logs.cmd", "collect_input.cmd"):
    data = (source / name).read_bytes().replace(b"\r\n", b"\n").replace(b"\r", b"\n")
    (output / name).write_bytes(data.replace(b"\n", b"\r\n"))
PY

cp "$wim_source" "$scratch/boot.wim"
cat > "$scratch/update.wimscript" <<EOF
delete /J316sInput/start_setup.cmd
add $root/tools/j316s_winpe/AppleInputLiveDiag.c /J316sInput/AppleInputLiveDiag.c
add $scratch/AppleInputLiveDiag.exe /J316sInput/AppleInputLiveDiag.exe
add $scratch/start_setup.cmd /J316sInput/start_setup.cmd
add $scratch/persist_logs.cmd /J316sInput/persist_logs.cmd
add $scratch/collect_input.cmd /J316sInput/collect_input.cmd
EOF
"$(command -v wimlib-imagex)" update "$scratch/boot.wim" 2 < "$scratch/update.wimscript"

cp "$media_source" "$scratch/setup-media.img"
mcopy -o -i "$scratch/setup-media.img" "$scratch/boot.wim" ::/boot.wim

PATH="/opt/homebrew/opt/llvm/bin:$PATH" python3 "$root/tools/pack_j316s_setup.py" \
    --mu-build "$root/mu/Build/J316sSetup-AARCH64/DEBUG_CLANGPDB" \
    --loader-elf "$root/m1n1_windows/build/m1n1-raw.elf" \
    --media "$scratch/setup-media.img" --output "$candidate" --usb --apple-input --diagnostics-slice

python3 - "$root" "$candidate" "$manifest" "$scratch/boot.wim" "$scratch/AppleInputLiveDiag.exe" "$rollback" "$diagnostics_disk" <<'PY'
import hashlib
import json
import sys
from pathlib import Path

root, candidate, manifest, wim, diag, rollback, diagnostics_disk = map(Path, sys.argv[1:])
digest = lambda path: hashlib.file_digest(path.open("rb"), "sha256").hexdigest()
data = {
    "profile": "J316s Windows Setup with persistent AppleInput diagnostics",
    "experiment": "J316S-APPLEINPUT-DIAG-001",
    "candidate": {"path": str(candidate.relative_to(root)), "sha256": digest(candidate)},
    "rollback": {"path": str(rollback.relative_to(root)), "sha256": digest(rollback)},
    "boot_wim_sha256": digest(wim),
    "live_diagnostic_sha256": digest(diag),
    "diagnostics_disk": {
        "path": str(diagnostics_disk.relative_to(root)),
        "sha256": digest(diagnostics_disk),
        "bytes": diagnostics_disk.stat().st_size,
        "host_slice": {"disk": "/dev/disk0s8", "gpt_index": 5,
                       "first_lba": 119146758, "blocks": 1567232,
                       "type_guid": "0509C4B9-AC59-4CCD-93F3-8DECB17F35E3",
                       "adjacent_boot_helper": {"device": "/dev/disk0s6",
                                                 "first_lba": 120713990,
                                                 "blocks": 32768}},
    },
    "mu_firmware_sha256": digest(root / "mu/Build/J316sSetup-AARCH64/DEBUG_CLANGPDB/FV/J316S_SETUP_EFI.fd"),
    "m1n1_loader_sha256": digest(root / "m1n1_windows/build/m1n1-raw.elf"),
    "log_destination": "J316S_DIAG volume marker J316S-DIAGNOSTICS.TARGET, J316sInputLogs/",
    "storage_profile": "bounded 1 GiB minimum GPT slice; no APFS or USB log writes",
    "diagnostics": ["drvload and SetupAPI installation logs", "PnP/resources/service state",
                    "AppleInput SPI, interrupt, HID discovery and VHF counters"],
    "safety_scope": "No host storage or firmware is modified by this build step.",
}
manifest.write_text(json.dumps(data, indent=2) + "\n")
print(json.dumps(data, indent=2))
PY
