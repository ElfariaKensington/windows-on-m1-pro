#!/bin/sh
# Build and sign the J316WifiCx package on macOS: build the driver, Authenticode-sign the
# .sys (SHA-256, page hashes), build the catalog with tools/make_driver_catalog.py (makecat
# format, verified byte-identical to a makecat catalog), sign the catalog, and verify both
# against the certificate. Signing key: .build/j316s-wificx/signing (CN=J316 WiFi Development).
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
K=.build/j316s-wificx/signing
SRC=drivers/j316-wificx
VER=$(sed -n 's/^DriverVer *= *[^,]*,\(.*\)$/\1/p' "$SRC/J316WifiCx.inf" | tr -d '\r ')
PKG=.build/j316s-wificx/package-$VER
[ -f "$K/key.pem" ] && [ -f "$K/cert.pem" ] && [ -f "$K/J316WifiCx.cer" ] || { echo "missing signing material in $K" >&2; exit 1; }
[ ! -e "$PKG" ] || { echo "$PKG exists; bump DriverVer in $SRC/J316WifiCx.inf" >&2; exit 1; }
bash "$SRC/build.sh" >/dev/null
mkdir -p "$PKG"
osslsigncode sign -ph -h sha256 -certs "$K/cert.pem" -key "$K/key.pem" -n "J316 Wi-Fi" \
    -in "$SRC/build/J316WifiCx.sys" -out "$PKG/J316WifiCx.sys" >/dev/null
cp "$SRC/J316WifiCx.inf" "$PKG/"
python3 -I tools/make_driver_catalog.py "$PKG/J316WifiCx-unsigned.cat" "$PKG/J316WifiCx.inf" "$PKG/J316WifiCx.sys"
osslsigncode sign -h sha256 -certs "$K/cert.pem" -key "$K/key.pem" \
    -in "$PKG/J316WifiCx-unsigned.cat" -out "$PKG/J316WifiCx.cat" >/dev/null
mv "$PKG/J316WifiCx-unsigned.cat" "$PKG/.unsigned.cat"
cp "$K/J316WifiCx.cer" "$SRC/switch-mode.ps1" "$SRC/README.md" "$PKG/"
osslsigncode verify -CAfile "$K/cert.pem" -in "$PKG/J316WifiCx.sys" | grep -q "Signature verification: ok" || { echo "sys signature check failed" >&2; exit 1; }
osslsigncode verify -CAfile "$K/cert.pem" -in "$PKG/J316WifiCx.cat" | grep -q "Signature verification: ok" || { echo "cat signature check failed" >&2; exit 1; }
osslsigncode verify -CAfile "$K/cert.pem" -catalog "$PKG/J316WifiCx.cat" -in "$PKG/J316WifiCx.sys" | grep -q "File is signed in catalog" || { echo "sys not in catalog" >&2; exit 1; }
python3 -I - "$PKG" <<'PY'
import hashlib, sys
from pathlib import Path
p = Path(sys.argv[1])
tag = (hashlib.sha1((p / "J316WifiCx.inf").read_bytes()).hexdigest().upper() + "\0").encode("utf-16-le")
assert tag in (p / "J316WifiCx.cat").read_bytes(), "INF hash missing from catalog"
PY
echo "$PKG" > .build/j316s-wificx/CURRENT
echo "OK: $PKG (sys, cat, INF membership verified against CN=J316 WiFi Development)"
shasum -a 256 "$PKG/J316WifiCx.sys" "$PKG/J316WifiCx.cat" "$PKG/J316WifiCx.inf" "$PKG/J316WifiCx.cer"
