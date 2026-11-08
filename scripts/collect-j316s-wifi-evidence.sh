#!/bin/sh
# Read-only: copy Wi-Fi driver evidence (driver log, minidumps, setupapi, System log) out of
# the installed Windows partition while macOS is booted. Nothing on the Windows disk is written.
set -eu
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT="$ROOT/.build/j316s-wificx/evidence/$(date -u +%Y%m%dT%H%M%SZ)"
OWNER=$(stat -f %Su "$ROOT")
ID=$(diskutil info -plist A37CB41C-BDF1-47E3-8F28-C8CEE41469EE | plutil -extract DeviceIdentifier raw -o - -)
case "$ID" in disk0s[0-9]*) ;; *) echo "unexpected device $ID" >&2; exit 1 ;; esac
DEV=$(hdiutil attach -readonly -nomount -imagekey diskimage-class=CRawDiskImage "/dev/$ID" | head -1 | awk '{print $1}')
cleanup() { [ -n "${MNT:-}" ] && diskutil unmount "$MNT" >/dev/null 2>&1; hdiutil detach "$DEV" >/dev/null 2>&1 || true; }
trap cleanup EXIT
diskutil list "$DEV"
NTFS=""
for s in $(diskutil list "$DEV" | awk '/'"$(basename "$DEV")"'s[0-9]+$/ {print $NF}'); do
    [ "$(diskutil info "$s" | awk -F: '/File System Personality/ {gsub(/ /,"",$2); print $2}')" = NTFS ] && NTFS=$s
done
[ -n "$NTFS" ] || { echo "no NTFS partition found inside $DEV" >&2; exit 1; }
MNT=$(mktemp -d /tmp/j316win.XXXX)
mount -t ntfs -o rdonly,nobrowse "/dev/$NTFS" "$MNT"
W="$MNT/Windows"
mkdir -p "$OUT"
cp "$W/Temp/J316WifiCx.log" "$OUT/" 2>/dev/null || echo "no J316WifiCx.log"
cp -R "$W/Minidump" "$OUT/" 2>/dev/null || echo "no Minidump dir"
ls -la "$W/MEMORY.DMP" 2>/dev/null && echo "(full MEMORY.DMP exists, not copied)"
cp -R "$W/LiveKernelReports" "$OUT/" 2>/dev/null || true
cp "$W/INF/setupapi.dev.log" "$OUT/" 2>/dev/null || true
cp "$W/System32/winevt/Logs/System.evtx" "$OUT/" 2>/dev/null || true
cp "$W/System32/winevt/Logs/Microsoft-Windows-WLAN-AutoConfig%4Operational.evtx" "$OUT/" 2>/dev/null || true
cp "$W/System32/DriverStore/FileRepository/"j316wifi*/*.inf "$OUT/" 2>/dev/null || true
ls -d "$W/System32/DriverStore/FileRepository/"j316* > "$OUT/driverstore.txt" 2>/dev/null || true
chown -R "$OWNER" "$OUT"
echo "EVIDENCE: $OUT"; ls -laR "$OUT" | head -60
