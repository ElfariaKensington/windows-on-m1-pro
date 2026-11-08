#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT="$ROOT/.build/j316s-diagnostics"
EXPECTED_EFI=2dd9323902f5039711eaeebd615d6346e62eb1438029e36cddd7e76aac50353b

for file in gpt-before.txt gpt-primary-before.bin gpt-secondary-before.bin efi-before.sha256; do
    [ ! -e "$OUT/$file" ] || {
        echo "Refusing to overwrite existing backup: $OUT/$file" >&2
        exit 1
    }
done

/usr/sbin/gpt show /dev/disk0 > "$OUT/gpt-before.txt"
/bin/dd if=/dev/rdisk0 bs=4096 count=7 of="$OUT/gpt-primary-before.bin" 2>/dev/null
/bin/dd if=/dev/rdisk0 bs=4096 skip=122138128 count=5 of="$OUT/gpt-secondary-before.bin" 2>/dev/null
EFI=$(/bin/dd if=/dev/rdisk0s4 bs=4096 count=244224 2>/dev/null | /usr/bin/shasum -a 256 | /usr/bin/awk '{print $1}')
[ "$EFI" = "$EXPECTED_EFI" ] || {
    echo "Unexpected EFI contents ($EFI); no disk writes were made" >&2
    exit 1
}
/usr/bin/shasum -a 256 "$OUT/gpt-primary-before.bin" "$OUT/gpt-secondary-before.bin" > "$OUT/gpt-before.sha256"
printf '%s  /dev/disk0s4\n' "$EFI" > "$OUT/efi-before.sha256"
cat "$OUT/gpt-before.txt"
cat "$OUT/gpt-before.sha256"
cat "$OUT/efi-before.sha256"
