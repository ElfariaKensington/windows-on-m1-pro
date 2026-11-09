#!/bin/sh
# Install the hash-pinned J316s-GPU ESP image onto the internal EFI partition.
# Before writing it saves the exact current ESP (unless it is the pinned Wi-Fi-017
# image), verifies the write by full readback and writes a receipt.
#   sudo sh scripts/install-j316s-gpu-esp.sh             install GPU image
#   sudo sh scripts/install-j316s-gpu-esp.sh --rollback  restore the known-good Wi-Fi-017 ESP
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DISK_UUID=9FBC60EC-E8FD-4270-8C54-B18BB7DDD4AE
SIZE=1000341504
NAME=J316s-GPU-003
GPU="$ROOT/.build/j316s-gpu/esp-$NAME.img"
GPU_SHA=588925e6abbe0ed4cbb5c95d9bda2499c298771aeb7c46c1959124566756593f
WIFI017="$ROOT/.build/j316s-wifi/esp-J316s-Wifi-017.img"
WIFI017_SHA=d4fe0e9108d612ccbb1751f6427c0838bf7eab714d7281eb2c0f834fa0ef8dfe
BACKUP="$ROOT/.build/j316s-gpu/esp-before-$NAME.img"
RECEIPT="$ROOT/dist/j316s/$NAME.manifest.installed.json"
STATE="$ROOT/.build/j316s-gpu/$NAME.pre-install.sha256"

[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
[ "$(sysctl -n hw.model)" = MacBookPro18,1 ] || { echo "unexpected Mac model" >&2; exit 1; }

PLIST=$(diskutil info -plist "$DISK_UUID")
ID=$(echo "$PLIST" | plutil -extract DeviceIdentifier raw -o - -)
CONTENT=$(echo "$PLIST" | plutil -extract Content raw -o - -)
INTERNAL=$(echo "$PLIST" | plutil -extract Internal raw -o - -)
TOTAL=$(echo "$PLIST" | plutil -extract TotalSize raw -o - -)
[ "$CONTENT" = EFI ] && [ "$INTERNAL" = true ] && [ "$TOTAL" = "$SIZE" ] || { echo "target is not the expected internal ESP" >&2; exit 1; }
case "$ID" in disk0s[0-9]*) ;; *) echo "unexpected device $ID" >&2; exit 1 ;; esac

current_sha() { dd if="/dev/r$ID" bs=4m count=$((SIZE / 4194304 + 1)) 2>/dev/null | head -c "$SIZE" | shasum -a 256 | cut -d' ' -f1; }
write_image() { # image sha
    [ "$(stat -f %z "$1")" = "$SIZE" ] || { echo "image size mismatch: $1" >&2; exit 1; }
    [ "$(shasum -a 256 "$1" | cut -d' ' -f1)" = "$2" ] || { echo "image hash mismatch: $1" >&2; exit 1; }
    diskutil unmount "$ID" >/dev/null 2>&1 || true
    dd if="$1" of="/dev/r$ID" bs=4m
    sync
    ACTUAL=$(current_sha)
    [ "$ACTUAL" = "$2" ] || { echo "READBACK MISMATCH: $ACTUAL" >&2; exit 1; }
}

diskutil unmount "$ID" >/dev/null 2>&1 || true
BEFORE=$(current_sha)

if [ "${1:-}" = "--rollback" ]; then
    # Wi-Fi-017 is the last image without GPU services; with it the AGX driver
    # finds no APPL0002 device and Windows uses Microsoft Basic Display.
    IMG=$WIFI017; WANT=$WIFI017_SHA
    write_image "$IMG" "$WANT"
    echo "OK: rolled back to $IMG, readback $ACTUAL"
    exit 0
fi

if [ "$BEFORE" != "$GPU_SHA" ]; then
    echo "$BEFORE" > "$STATE"
    if [ "$BEFORE" != "$WIFI017_SHA" ]; then
        echo "current ESP is not Wi-Fi-017 ($BEFORE); saving it to $BACKUP"
        dd if="/dev/r$ID" bs=4m count=$((SIZE / 4194304 + 1)) 2>/dev/null | head -c "$SIZE" > "$BACKUP"
        [ "$(shasum -a 256 "$BACKUP" | cut -d' ' -f1)" = "$BEFORE" ] || { echo "backup readback mismatch" >&2; exit 1; }
    fi
fi
write_image "$GPU" "$GPU_SHA"

cat > "$RECEIPT" <<EOF
{
  "time_utc": "$(date -u +%Y-%m-%dT%H:%M:%S+00:00)",
  "device": "/dev/r$ID",
  "disk_uuid": "$DISK_UUID",
  "bytes": $SIZE,
  "action": "installed",
  "pre_install_sha256": "$(cat "$STATE" 2>/dev/null || echo unknown)",
  "readback_sha256": "$ACTUAL",
  "native_test": "pending user boot"
}
EOF
SUDO_UID=${SUDO_UID:-}
[ -n "$SUDO_UID" ] && chown "$SUDO_UID" "$RECEIPT" "$STATE" 2>/dev/null || true
echo "OK: installed $NAME, pre-install $(cat "$STATE" 2>/dev/null), readback $ACTUAL"
