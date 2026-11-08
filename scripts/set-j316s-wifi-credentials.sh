#!/bin/sh
# Prompt for the Wi-Fi SSID/PSK on the Mac and write the local-only header that the
# hypervisor embeds for boot-time join. Nothing is echoed; file is mode 0600 and lives
# next to the proprietary firmware header (never in the source tree).
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT="$ROOT/.build/j316s-wifi/local/wifi_credentials.h"
printf 'Wi-Fi SSID: '; IFS= read -r SSID
printf 'Wi-Fi password (hidden): '; stty -echo; IFS= read -r PSK; stty echo; printf '\n'
SSID="$SSID" PSK="$PSK" OUT="$OUT" python3 -I - <<'PY'
import os
ssid=os.environ['SSID'].encode(); psk=os.environ['PSK'].encode()
assert 1<=len(ssid)<=32, "SSID must be 1-32 bytes"
assert (8<=len(psk)<=63 and all(32<=c<127 for c in psk)) or (len(psk)==64 and all(chr(c) in "0123456789abcdefABCDEF" for c in psk)), "PSK must be 8-63 printable ASCII chars or 64 hex"
fmt=lambda b: ",".join(str(c) for c in b)+",0"
out=os.environ['OUT']; fd=os.open(out,os.O_CREAT|os.O_TRUNC|os.O_WRONLY,0o600)
with os.fdopen(fd,'w') as f:
    f.write("/* Private generated file; contains a Wi-Fi credential. Never commit or redistribute. */\n")
    f.write("static const u8 j316_wifi_ssid[] = {%s};\nstatic const u8 j316_wifi_psk[] = {%s};\n"%(fmt(ssid),fmt(psk)))
os.chmod(out,0o600)
print("credentials stored in", out)
PY
