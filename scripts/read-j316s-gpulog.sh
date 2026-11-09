#!/bin/sh
# Copy the m1n1 GPU probe log off the internal ESP (raw read of its fixed 8 MiB extent) and decode it.
#   sudo sh scripts/read-j316s-gpulog.sh [J316s-GPU-NNN]
# Output: .build/j316s-gpu/evidence/<name>-<UTC time>/{gpulog.bin,summary.json,probe.txt,console.txt,adt.bin,adt.txt,adt-gpu.json}
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
NAME=${1:-J316s-GPU-003}
DISK_UUID=9FBC60EC-E8FD-4270-8C54-B18BB7DDD4AE
META="$ROOT/.build/j316s-gpu/$NAME.gpulog.json"
OUT="$ROOT/.build/j316s-gpu/evidence/$NAME-$(date -u +%Y%m%dT%H%M%SZ)"
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }

OFFSET=$(plutil -extract file_offset raw -o - "$META")
BYTES=$(plutil -extract file_bytes raw -o - "$META")
ID=$(diskutil info -plist "$DISK_UUID" | plutil -extract DeviceIdentifier raw -o - -)
case "$ID" in disk0s[0-9]*) ;; *) echo "unexpected device $ID" >&2; exit 1 ;; esac
mkdir -p "$OUT"
dd if="/dev/r$ID" of="$OUT/gpulog.bin" bs=4096 skip=$((OFFSET / 4096)) count=$((BYTES / 4096)) 2>/dev/null
python3 -I "$ROOT/tools/j316s_gpulog.py" decode "$OUT/gpulog.bin" --meta "$META" --out "$OUT"
[ -n "${SUDO_UID:-}" ] && chown -R "$SUDO_UID" "$OUT" || true
echo "$OUT"
