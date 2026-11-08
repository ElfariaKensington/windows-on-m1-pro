#!/bin/sh
# One command: ask for Wi-Fi credentials, rebuild/pack, then install to the ESP (sudo).
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
[ -f "$ROOT/.build/j316s-wifi/local/wifi_credentials.h" ] && [ "${1:-}" = "--keep-credentials" ] || "$ROOT/scripts/set-j316s-wifi-credentials.sh"
"$ROOT/scripts/build-j316s-wifi.sh"
sudo "$ROOT/scripts/install-j316s-wifi-esp.sh"
echo "Done. Reboot into the Apple boot-picker entry; the Wi-Fi result is printed on the boot screen."
