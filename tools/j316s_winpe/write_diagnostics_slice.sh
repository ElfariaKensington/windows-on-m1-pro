#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TARGET=/dev/disk0s8
RAW=/dev/rdisk0s8
IMAGE="$ROOT/.build/j316s-diagnostics/diagnostics-disk-v2.img"
EXPECTED_IMAGE=1b09497fe447228ae6e63859f37e05656a5c95f976ebd192868fa8c736b6a134
INFO="$ROOT/.build/j316s-diagnostics/storage-target-info.txt"
PLIST="$ROOT/.build/j316s-diagnostics/storage-target.plist"

[ "$(/usr/bin/shasum -a 256 "$IMAGE" | /usr/bin/awk '{print $1}')" = "$EXPECTED_IMAGE" ]
/usr/sbin/diskutil info -plist "$TARGET" > "$PLIST"
/usr/sbin/diskutil info "$TARGET" > "$INFO"
/usr/bin/grep -Fq 'Partition Type:            0509C4B9-AC59-4CCD-93F3-8DECB17F35E3' "$INFO"
/usr/bin/grep -Fq 'Partition Offset:          488025120768 Bytes (119146758 4096-Byte-Device-Blocks)' "$INFO"
/usr/bin/grep -Fq 'Disk Size:                 6.4 GB (6419382272 Bytes)' "$INFO"
/usr/bin/grep -Fq 'Device Block Size:         4096 Bytes' "$INFO"
/usr/bin/grep -Fq 'Device Location:           Internal' "$INFO"

/bin/dd if="$IMAGE" of="$RAW" bs=4096 count=1567232
/bin/sync
READBACK=$(/bin/dd if="$RAW" bs=4096 count=1567232 2>/dev/null | /usr/bin/shasum -a 256 | /usr/bin/awk '{print $1}')
[ "$READBACK" = "$EXPECTED_IMAGE" ] || {
    echo "Diagnostic-slice full readback mismatch" >&2
    exit 1
}
printf 'SLICE_WRITTEN %s\n' "$READBACK"
