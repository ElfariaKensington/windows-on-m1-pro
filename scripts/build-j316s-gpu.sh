#!/bin/sh
# Build J316s-GPU-NNN: the installed Wi-Fi-017 image plus the m1n1 T6000 AGX probe
# (J316S_GPU_PROBE=1), with a contiguous ::m1n1/gpulog.bin in the ESP for its log.
# Never touches a disk. Install with scripts/install-j316s-gpu-esp.sh.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
export PATH=$HOME/.rustup/toolchains/stable-aarch64-apple-darwin/bin:/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:$PATH
W=.build/j316s-gpu
BASE=.build/j316s-wifi/esp-J316s-Wifi-017.img
BASE_SHA=d4fe0e9108d612ccbb1751f6427c0838bf7eab714d7281eb2c0f834fa0ef8dfe
PART_UUID=9FBC60EC-E8FD-4270-8C54-B18BB7DDD4AE
PART_START_LBA=118902534
PART_BLOCKS=244224
mkdir -p "$W/local"

[ "$(shasum -a 256 "$BASE" | cut -d' ' -f1)" = "$BASE_SHA" ] || { echo "base ESP hash mismatch" >&2; exit 1; }

N=1; while [ -e "dist/j316s/J316s-GPU-$(printf %03d $N).bin" ]; do N=$((N+1)); done
NAME="J316s-GPU-$(printf %03d $N)"
IMG="$W/esp-$NAME.img"
cp "$BASE" "$IMG"

python3 -I tools/j316s_gpulog.py prepare "$IMG" --header "$W/local/j316s_gpulog_location.h" \
  --part-uuid "$PART_UUID" --part-start-lba "$PART_START_LBA" --part-blocks "$PART_BLOCKS" \
  --build "$NAME"
cp "$W/local/j316s_gpulog_location.json" "$W/$NAME.gpulog.json"

(cd m1n1_windows && make clean >/dev/null && cp ../.build/j316s-gpu/librust.a build/ 2>/dev/null || true)
(cd m1n1_windows && make J316S_SETUP=1 J316S_WIFI=1 J316S_GPU=1 J316S_GPU_PROBE=1) > "$W/m1n1-$NAME.log" 2>&1 || {
  tail -40 "$W/m1n1-$NAME.log" >&2; exit 1; }
grep -qx '#define M1N1_J316S_GPU_PROBE' m1n1_windows/build/build_cfg.h || { echo "probe flag missing" >&2; exit 1; }
grep -qx '#define M1N1_J316S_GPU' m1n1_windows/build/build_cfg.h || { echo "GPU flag missing" >&2; exit 1; }
strings -a mu/Build/J316sSetup-AARCH64/DEBUG_CLANGPDB/FV/J316S_SETUP_EFI.fd | grep -q APPL0002 || strings -a mu/Build/J316sSetup-AARCH64/DEBUG_CLANGPDB/AARCH64/J316sSetupPkg/AcpiTables/J316sStorageTables/OUTPUT/DSDT.aml | grep -q APPL0002 || { echo "Mu lacks APPL0002" >&2; exit 1; }
nm m1n1_windows/build/m1n1-raw.elf 2>/dev/null | grep -q hv_j316s_gpu_probe_run || \
  /opt/homebrew/opt/llvm/bin/llvm-nm m1n1_windows/build/m1n1-raw.elf | grep -q hv_j316s_gpu_probe_run || \
  { echo "probe symbol missing from m1n1" >&2; exit 1; }

python3 -I tools/pack_j316s_setup.py --mu-build mu/Build/J316sSetup-AARCH64/DEBUG_CLANGPDB \
  --loader-elf m1n1_windows/build/m1n1-raw.elf --media .build/j316s-wifi/setup.img \
  --output "dist/j316s/$NAME.bin" --usb --apple-input --windows-slice --wifi

mcopy -o -i "$IMG" "dist/j316s/$NAME.bin" ::m1n1/boot.bin
mcopy -i "$IMG" ::m1n1/boot.bin - | cmp - "dist/j316s/$NAME.bin"
python3 -I tools/j316s_gpulog.py verify "$IMG" --meta "$W/$NAME.gpulog.json"
[ "$(stat -f %z "$IMG")" = 1000341504 ] || { echo "ESP image size changed" >&2; exit 1; }
SHA=$(shasum -a 256 "$IMG" | cut -d' ' -f1)
BIN_SHA=$(shasum -a 256 "dist/j316s/$NAME.bin" | cut -d' ' -f1)
cat > "$W/$NAME.build.json" <<EOF
{"name": "$NAME", "esp_image": "$IMG", "esp_sha256": "$SHA",
 "boot_bin": "dist/j316s/$NAME.bin", "boot_bin_sha256": "$BIN_SHA",
 "base_esp": "$BASE", "base_esp_sha256": "$BASE_SHA",
 "m1n1_head": "$(git -C m1n1_windows rev-parse HEAD)", "m1n1_dirty": true}
EOF
echo "built $NAME esp=$IMG sha=$SHA boot.bin=$BIN_SHA"
