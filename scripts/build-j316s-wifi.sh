#!/bin/sh
# Rebuild m1n1 (Wi-Fi), pack the next J316s-Wifi-NNN.bin, build its ESP image from the
# previous one, and re-pin the install script (rollback stays the known-good input-recovery ESP). Never touches a disk.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
export PATH=/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:$PATH
W=.build/j316s-wifi
INSTALLER=scripts/install-j316s-wifi-esp.sh

touch m1n1_windows/src/hv_j316s_wifi.c
(cd m1n1_windows && make J316S_SETUP=1 J316S_WIFI=1)

N=1; while [ -e "dist/j316s/J316s-Wifi-$(printf %03d $N).bin" ]; do N=$((N+1)); done
NAME="J316s-Wifi-$(printf %03d $N)"
CUR_IMG=$(sed -n 's/^WIFI="\$ROOT\/\(.*\)"$/\1/p' $INSTALLER)

python3 -I tools/pack_j316s_setup.py --mu-build mu/Build/J316sSetup-AARCH64/DEBUG_CLANGPDB \
  --loader-elf m1n1_windows/build/m1n1-raw.elf --media $W/setup.img \
  --output "dist/j316s/$NAME.bin" --usb --apple-input --windows-slice --wifi

NEW_IMG="$W/esp-$NAME.img"
cp "$CUR_IMG" "$NEW_IMG"
mcopy -o -i "$NEW_IMG" "dist/j316s/$NAME.bin" ::m1n1/boot.bin
NEW_SHA=$(shasum -a 256 "$NEW_IMG" | cut -d' ' -f1)
mcopy -i "$NEW_IMG" ::m1n1/boot.bin - | cmp - "dist/j316s/$NAME.bin"

sed -i '' -e "s#^WIFI=.*#WIFI=\"\$ROOT/$NEW_IMG\"#" -e "s#^WIFI_SHA=.*#WIFI_SHA=$NEW_SHA#" \
  -e "s#^RECEIPT=.*#RECEIPT=\"\$ROOT/dist/j316s/$NAME.manifest.installed.json\"#" $INSTALLER
sh -n $INSTALLER
echo "built $NAME  esp=$NEW_IMG  sha=$NEW_SHA  rollback=unchanged (input-recovery)"
