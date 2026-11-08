#!/bin/sh
# Build and test-sign the J316Wifi Ethernet bridge package (Ethernet mode of the Wi-Fi mode
# switch). Same signing identity and catalog path as scripts/package-j316s-wificx.sh.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
K=.build/j316s-wificx/signing
SRC=drivers/j316-wifi
VER=$(sed -n 's/^DriverVer *= *[^,]*,\(.*\)$/\1/p' "$SRC/J316Wifi.inf" | tr -d '\r ')
PKG=.build/j316s-wifi-bridge/package-$VER
[ -f "$K/key.pem" ] && [ -f "$K/cert.pem" ] && [ -f "$K/J316WifiCx.cer" ] || { echo "missing signing material in $K" >&2; exit 1; }
rm -rf "$PKG"
bash "$SRC/build.sh" >/dev/null
mkdir -p "$PKG"
osslsigncode sign -ph -h sha256 -certs "$K/cert.pem" -key "$K/key.pem" -n "J316 Wi-Fi Bridge" \
    -in "$SRC/build/J316Wifi.sys" -out "$PKG/J316Wifi.sys" >/dev/null
cp "$SRC/J316Wifi.inf" "$SRC/configure.ps1" "$PKG/"
python3 -I tools/make_driver_catalog.py "$PKG/J316Wifi-unsigned.cat" "$PKG/J316Wifi.inf" "$PKG/J316Wifi.sys"
osslsigncode sign -h sha256 -certs "$K/cert.pem" -key "$K/key.pem" \
    -in "$PKG/J316Wifi-unsigned.cat" -out "$PKG/J316Wifi.cat" >/dev/null
rm "$PKG/J316Wifi-unsigned.cat"
osslsigncode verify -CAfile "$K/cert.pem" -in "$PKG/J316Wifi.sys" | grep -q "Signature verification: ok" || { echo "sys signature check failed" >&2; exit 1; }
osslsigncode verify -CAfile "$K/cert.pem" -in "$PKG/J316Wifi.cat" | grep -q "Signature verification: ok" || { echo "cat signature check failed" >&2; exit 1; }
osslsigncode verify -CAfile "$K/cert.pem" -catalog "$PKG/J316Wifi.cat" -in "$PKG/J316Wifi.sys" | grep -q "File is signed in catalog" || { echo "sys not in catalog" >&2; exit 1; }
python3 -I - "$PKG" <<'PY'
import hashlib, sys
from pathlib import Path
p = Path(sys.argv[1])
tag = (hashlib.sha1((p / "J316Wifi.inf").read_bytes()).hexdigest().upper() + "\0").encode("utf-16-le")
assert tag in (p / "J316Wifi.cat").read_bytes(), "INF hash missing from catalog"
PY
echo "$PKG" > .build/j316s-wifi-bridge/CURRENT
echo "OK: $PKG"
shasum -a 256 "$PKG/J316Wifi.sys" "$PKG/J316Wifi.cat" "$PKG/J316Wifi.inf"
