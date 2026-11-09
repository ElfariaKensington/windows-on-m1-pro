#!/bin/sh
# Build, sign (CN=J316 WiFi Development, already trusted by the J316 Windows install)
# and catalog the J316s AGX driver package: KMD + ARM64 UMD + INF.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
K=.build/j316s-wificx/signing
SRC=drivers/apple-agx/render-admission
VER=$(sed -n 's/^DriverVer *= *[^,]*,\(.*\)$/\1/p' "$SRC/AppleAgxJ316s.inf" | tr -d '\r ')
PKG=.build/j316s-agx/package-$VER
[ -f "$K/key.pem" ] && [ -f "$K/cert.pem" ] && [ -f "$K/J316WifiCx.cer" ] || { echo "missing signing material" >&2; exit 1; }
rm -rf "$PKG"; mkdir -p "$PKG"
(cd "$SRC" && PLATFORM=j316s ./build-clang.sh >/dev/null)
(cd "$SRC/umd" && ./build-clang.sh >/dev/null)
for f in AppleAgxRenderAdmission.sys:kmd-j316s AppleAgxRenderAdmissionUmd.dll:umd-j316s; do
  n=${f%:*}; d=${f#*:}
  osslsigncode sign -ph -h sha256 -certs "$K/cert.pem" -key "$K/key.pem" -n "Apple AGX J316s" \
    -in ".build/j316s-agx/$d/$n" -out "$PKG/$n" >/dev/null
  osslsigncode verify -CAfile "$K/cert.pem" -in "$PKG/$n" | grep -q "Signature verification: ok" || { echo "$n signature check failed" >&2; exit 1; }
done
cp "$SRC/AppleAgxJ316s.inf" "$PKG/"
python3 -I tools/make_driver_catalog.py "$PKG/.unsigned.cat" "$PKG/AppleAgxJ316s.inf" "$PKG/AppleAgxRenderAdmission.sys" "$PKG/AppleAgxRenderAdmissionUmd.dll"
osslsigncode sign -h sha256 -certs "$K/cert.pem" -key "$K/key.pem" -in "$PKG/.unsigned.cat" -out "$PKG/AppleAgxJ316s.cat" >/dev/null
for n in AppleAgxRenderAdmission.sys AppleAgxRenderAdmissionUmd.dll; do
  osslsigncode verify -CAfile "$K/cert.pem" -catalog "$PKG/AppleAgxJ316s.cat" -in "$PKG/$n" | grep -q "File is signed in catalog" || { echo "$n not in catalog" >&2; exit 1; }
done
cp "$K/J316WifiCx.cer" "$PKG/J316Development.cer"
echo "$PKG" > .build/j316s-agx/CURRENT
echo "OK: $PKG"; shasum -a 256 "$PKG"/*.sys "$PKG"/*.dll "$PKG"/*.cat "$PKG"/*.inf
