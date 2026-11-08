#!/bin/sh
# Install the hash-pinned J316s Wi-Fi ESP image onto the internal EFI partition,
# verify by full readback, and write the .installed.json receipt.
# Rollback: sudo scripts/install-j316s-wifi-esp.sh --rollback  (restores the known-good input-recovery ESP, no Wi-Fi)
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DISK_UUID=9FBC60EC-E8FD-4270-8C54-B18BB7DDD4AE
SIZE=1000341504
WIFI="$ROOT/.build/j316s-wifi/esp-J316s-Wifi-012.img"
WIFI_SHA=dbc5be260d0f87b6f0bb461ddf76bc9539a4feb61ec42f37f377f1ff1dce2acf
ROLLBACK="$ROOT/.build/j316s-install/esp-input-recovery.img"
ROLLBACK_SHA=725b6e5d78575fc96e842fd6bf33f104e0e34733186904369e35167b4ecb2a8c
RECEIPT="$ROOT/dist/j316s/J316s-Wifi-012.manifest.installed.json"

[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
[ "$(sysctl -n hw.model)" = MacBookPro18,1 ] || { echo "unexpected Mac model" >&2; exit 1; }

IMAGE=$WIFI; SHA=$WIFI_SHA; ACTION=installed
if [ "${1:-}" = "--rollback" ]; then IMAGE=$ROLLBACK; SHA=$ROLLBACK_SHA; ACTION=rolled-back; fi

PLIST=$(diskutil info -plist "$DISK_UUID")
ID=$(echo "$PLIST" | plutil -extract DeviceIdentifier raw -o - -)
CONTENT=$(echo "$PLIST" | plutil -extract Content raw -o - -)
INTERNAL=$(echo "$PLIST" | plutil -extract Internal raw -o - -)
TOTAL=$(echo "$PLIST" | plutil -extract TotalSize raw -o - -)
[ "$CONTENT" = EFI ] && [ "$INTERNAL" = true ] && [ "$TOTAL" = "$SIZE" ] || { echo "target is not the expected internal ESP" >&2; exit 1; }
case "$ID" in disk0s[0-9]*) ;; *) echo "unexpected device $ID" >&2; exit 1 ;; esac

[ "$(stat -f %z "$IMAGE")" = "$SIZE" ] || { echo "image size mismatch" >&2; exit 1; }
[ "$(shasum -a 256 "$IMAGE" | cut -d' ' -f1)" = "$SHA" ] || { echo "image hash mismatch" >&2; exit 1; }

diskutil unmount "$ID" >/dev/null 2>&1 || true
dd if="$IMAGE" of="/dev/r$ID" bs=4m
sync
ACTUAL=$(dd if="/dev/r$ID" bs=4m count=$((SIZE / 4194304 + 1)) 2>/dev/null | head -c "$SIZE" | shasum -a 256 | cut -d' ' -f1)
[ "$ACTUAL" = "$SHA" ] || { echo "READBACK MISMATCH: $ACTUAL" >&2; exit 1; }

cat > "$RECEIPT" <<EOF
{
  "time_utc": "$(date -u +%Y-%m-%dT%H:%M:%S+00:00)",
  "device": "/dev/r$ID",
  "disk_uuid": "$DISK_UUID",
  "bytes": $SIZE,
  "action": "$ACTION",
  "readback_sha256": "$ACTUAL",
  "native_wifi_test": "pending user test"
}
EOF
echo "OK: $ACTION, readback $ACTUAL"
