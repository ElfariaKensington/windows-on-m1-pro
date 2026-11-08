#!/bin/sh
# Stage the Wi-Fi mode switch on the FAT32 J316SOURCE partition inside the private Windows
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
[ -f "$ROOT/.build/j316s-wificx/CURRENT" ] || { echo "run scripts/package-j316s-wificx.sh first" >&2; exit 1; }
[ -f "$ROOT/.build/j316s-wifi-bridge/CURRENT" ] || { echo "run scripts/package-j316s-wifi-bridge.sh first" >&2; exit 1; }
PKG="$ROOT/$(cat "$ROOT/.build/j316s-wificx/CURRENT")"
BRIDGE="$ROOT/$(cat "$ROOT/.build/j316s-wifi-bridge/CURRENT")"
WIFI_FILES="J316WifiCx.inf J316WifiCx.sys J316WifiCx.cat"
ETH_FILES="J316Wifi.inf J316Wifi.sys J316Wifi.cat configure.ps1"
for f in $WIFI_FILES J316WifiCx.cer switch-mode.ps1 README.md; do [ -f "$PKG/$f" ] || { echo "missing $PKG/$f" >&2; exit 1; }; done
for f in $ETH_FILES; do [ -f "$BRIDGE/$f" ] || { echo "missing $BRIDGE/$f" >&2; exit 1; }; done

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

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
mkdir "$STAGE/wifi" "$STAGE/ethernet"
for f in $WIFI_FILES; do cp "$PKG/$f" "$STAGE/wifi/"; done
for f in $ETH_FILES; do cp "$BRIDGE/$f" "$STAGE/ethernet/"; done
cp "$PKG/J316WifiCx.cer" "$PKG/switch-mode.ps1" "$PKG/README.md" "$STAGE/"
mkcmd() {  # name, mode, comment
    printf '@echo off\r\nrem %s\r\nrem Right-click > Run as administrator.\r\ncd /d "%%~dp0"\r\npowershell -NoProfile -ExecutionPolicy Bypass -File "%%~dp0switch-mode.ps1" %s\r\npause\r\n' "$3" "$2" > "$STAGE/$1"
}
mkcmd "Wi-Fi Mode.cmd" menu "Choose Wi-Fi (native adapter) or Ethernet (m1n1 bridge) mode."
mkcmd "Use Wi-Fi.cmd" wifi "Native Wi-Fi adapter: connect from Settings."
mkcmd "Use Ethernet.cmd" ethernet "Ethernet bridge: m1n1 joins Wi-Fi, Windows sees a wired adapter."
TOP="J316WifiCx.cer switch-mode.ps1 README.md"
CMDS="Wi-Fi Mode.cmd|Use Wi-Fi.cmd|Use Ethernet.cmd"
# mtools prompts on name clashes; never let it wait on the terminal.
for d in ::/J316WifiCx ::/J316WifiCx/wifi ::/J316WifiCx/ethernet; do
    mdir -i "$TARGET" "$d" </dev/null >/dev/null 2>&1 || mmd -i "$TARGET" "$d" </dev/null
done
# Remove files of earlier layouts (a development key, the old single-mode installer).
for old in J316Dev.pfx "Install Wi-Fi.cmd" install.ps1 J316WifiCx.inf J316WifiCx.sys J316WifiCx.cat; do
    if mdir -i "$TARGET" "::/J316WifiCx/$old" </dev/null >/dev/null 2>&1; then
        mdel -i "$TARGET" "::/J316WifiCx/$old" </dev/null
    fi
done
copy() { mcopy -o -i "$TARGET" "$STAGE/$1" "::/J316WifiCx/$1" </dev/null; }
check() { mcopy -i "$TARGET" "::/J316WifiCx/$1" - </dev/null | cmp - "$STAGE/$1" || { echo "READBACK MISMATCH: $1" >&2; exit 1; }; }
for f in $TOP; do copy "$f"; done
for f in $WIFI_FILES; do copy "wifi/$f"; done
for f in $ETH_FILES; do copy "ethernet/$f"; done
IFS='|'; for f in $CMDS; do copy "$f"; done; unset IFS
sync
for f in $TOP; do check "$f"; done
for f in $WIFI_FILES; do check "wifi/$f"; done
for f in $ETH_FILES; do check "ethernet/$f"; done
IFS='|'; for f in $CMDS; do check "$f"; done; unset IFS
echo "OK: staged on J316SOURCE ::/J316WifiCx (readback verified)"
echo "  wifi/J316WifiCx.sys   $(shasum -a 256 "$PKG/J316WifiCx.sys" | cut -d' ' -f1)"
echo "  ethernet/J316Wifi.sys $(shasum -a 256 "$BRIDGE/J316Wifi.sys" | cut -d' ' -f1)"
echo "In Windows: open the J316SOURCE drive, J316WifiCx folder, right-click 'Wi-Fi Mode.cmd' > Run as administrator"
