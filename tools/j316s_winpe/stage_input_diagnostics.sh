#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TARGET=/dev/disk0s4
RAW=/dev/rdisk0s4
IMAGE="$ROOT/.build/j316s-esp-usb-input/j316s-input-diagnostics-v3.img"
EXPECTED_OLD=2dd9323902f5039711eaeebd615d6346e62eb1438029e36cddd7e76aac50353b
EXPECTED_NEW=81890d544ab01bee007cfbb1bff0271225ec20d69ff4b33ec74b4223b269479a
EXPECTED_UUID=9FBC60EC-E8FD-4270-8C54-B18BB7DDD4AE
EXPECTED_SIZE=1000341504
INFO="$ROOT/.build/j316s-diagnostics/efi-target-info.txt"
PLIST="$ROOT/.build/j316s-diagnostics/efi-target.plist"

[ "$(/usr/bin/shasum -a 256 "$IMAGE" | /usr/bin/awk '{print $1}')" = "$EXPECTED_NEW" ]
/usr/sbin/diskutil info -plist "$TARGET" > "$PLIST"
/usr/sbin/diskutil info "$TARGET" > "$INFO"
[ "$(/usr/bin/plutil -extract DiskUUID raw -o - "$PLIST")" = "$EXPECTED_UUID" ]
[ "$(/usr/bin/plutil -extract IOKitSize raw -o - "$PLIST")" = "$EXPECTED_SIZE" ]
/usr/bin/grep -Eq '^[[:space:]]*Mounted:[[:space:]]*No$' "$INFO"
/usr/bin/grep -Eq '^[[:space:]]*File System Personality:[[:space:]]*MS-DOS FAT32$' "$INFO"
/usr/bin/grep -Eq '^[[:space:]]*Device Location:[[:space:]]*Internal$' "$INFO"

CURRENT=$(/bin/dd if="$RAW" bs=4096 count=244224 2>/dev/null | /usr/bin/shasum -a 256 | /usr/bin/awk '{print $1}')
[ "$CURRENT" = "$EXPECTED_OLD" ] || {
    echo "Refusing EFI write: current bytes do not match the staged interactive rollback image" >&2
    exit 1
}
/bin/dd if="$IMAGE" of="$RAW" bs=4096 count=244224
/bin/sync
READBACK=$(/bin/dd if="$RAW" bs=4096 count=244224 2>/dev/null | /usr/bin/shasum -a 256 | /usr/bin/awk '{print $1}')
[ "$READBACK" = "$EXPECTED_NEW" ] || {
    echo "EFI full-partition readback mismatch; do not reboot" >&2
    exit 1
}
printf 'STAGED %s\n' "$READBACK"
