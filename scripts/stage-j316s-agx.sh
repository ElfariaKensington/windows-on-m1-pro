#!/bin/sh
# (derived from stage-j316s-wificx.sh) Stage the Wi-Fi mode switch on the FAT32 J316SOURCE partition inside the private Windows
# container: native Wi-Fi (J316WifiCx) and Ethernet bridge (J316Wifi) packages plus
# "Wi-Fi Mode.cmd" / "Use Wi-Fi.cmd" / "Use Ethernet.cmd".
# Run with sudo while macOS is booted (Windows is not running). Only files under ::/J316WifiCx
# on the verified source partition are written.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
LAYOUT="$ROOT/.build/j316s-install/windows-source-disk.layout.json"
DISK_UUID=A37CB41C-BDF1-47E3-8F28-C8CEE41469EE
BLOCK=4096

[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
[ "$(sysctl -n hw.model)" = MacBookPro18,1 ] || { echo "unexpected Mac model" >&2; exit 1; }

PLIST=$(diskutil info -plist "$DISK_UUID")
ID=$(echo "$PLIST" | plutil -extract DeviceIdentifier raw -o - -)
INTERNAL=$(echo "$PLIST" | plutil -extract Internal raw -o - -)
TOTAL=$(echo "$PLIST" | plutil -extract TotalSize raw -o - -)
[ "$INTERNAL" = true ] && [ "$TOTAL" = 79865782272 ] || { echo "target is not the expected private Windows container" >&2; exit 1; }
case "$ID" in disk0s[0-9]*) ;; *) echo "unexpected device $ID" >&2; exit 1 ;; esac
MOUNTED=$(echo "$PLIST" | plutil -extract Mounted raw -o - - 2>/dev/null || echo false)
MOUNTPOINT=$(echo "$PLIST" | plutil -extract MountPoint raw -o - - 2>/dev/null || true)
[ "$MOUNTED" = false ] && [ -z "$MOUNTPOINT" ] || { echo "container is mounted" >&2; exit 1; }

OFFSET=$(python3 -I - "$LAYOUT" "$BLOCK" <<'PY'
import json, sys
layout = json.load(open(sys.argv[1]))
part = [p for p in layout["partitions"] if p["name"] == "J316SOURCE"][0]
print(part["first"] * int(sys.argv[2]))
PY
)
TARGET="/dev/$ID@@$OFFSET"
# The partition must carry the installer marker (disk GUID) created by tools/j316s_install/layout.py.
GUID=$(python3 -I -c "import json,sys;print(json.load(open(sys.argv[1]))['disk_guid'])" "$LAYOUT")
[ "$(mcopy -i "$TARGET" ::/J316-INSTALL.SOURCE - | tr -d '\r\n')" = "$GUID" ] || { echo "J316SOURCE marker mismatch; nothing written" >&2; exit 1; }

PKG="$ROOT/$(cat "$ROOT/.build/j316s-agx/CURRENT")"
J="$ROOT/drivers/apple-agx/render-admission/j316s"
FILES="AppleAgxJ316s.inf AppleAgxRenderAdmission.sys AppleAgxRenderAdmissionUmd.dll AppleAgxJ316s.cat J316Development.cer"
for f in $FILES; do [ -f "$PKG/$f" ] || { echo "missing $PKG/$f" >&2; exit 1; }; done
mdir -i "$TARGET" ::/AppleGPU </dev/null >/dev/null 2>&1 || mmd -i "$TARGET" ::/AppleGPU </dev/null
mdir -i "$TARGET" ::/AppleGPU/logs </dev/null >/dev/null 2>&1 || mmd -i "$TARGET" ::/AppleGPU/logs </dev/null
for f in $FILES; do mcopy -o -i "$TARGET" "$PKG/$f" "::/AppleGPU/$f" </dev/null; done
mcopy -o -i "$TARGET" "$J/gpu.ps1" ::/AppleGPU/gpu.ps1 </dev/null
mcopy -o -i "$TARGET" "$J/Install Apple GPU.cmd" "::/AppleGPU/Install Apple GPU.cmd" </dev/null
mcopy -o -i "$TARGET" "$J/Remove Apple GPU.cmd" "::/AppleGPU/Remove Apple GPU.cmd" </dev/null
mcopy -o -i "$TARGET" "$J/Install Apple GPU.cmd" "::/Install Apple GPU.cmd" </dev/null
for f in $FILES; do mcopy -i "$TARGET" "::/AppleGPU/$f" - </dev/null | cmp - "$PKG/$f"; done
echo "OK: staged $PKG to J316SOURCE ::/AppleGPU (readback verified)"
