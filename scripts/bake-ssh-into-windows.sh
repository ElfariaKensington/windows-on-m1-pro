#!/bin/sh
# Stage the invoking user's GitHub SSH identity on the FAT32 J316SOURCE partition inside the
# private Windows container, so the installed Windows can import it with J316Ssh\install-ssh.cmd.
# Run with sudo while macOS is booted (Windows is not running). The private key is read from the
# user's ~/.ssh at run time and is never written into this repository. Only files under
# ::/J316Ssh on the verified source partition are written.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
LAYOUT="$ROOT/.build/j316s-install/windows-source-disk.layout.json"
DISK_UUID=A37CB41C-BDF1-47E3-8F28-C8CEE41469EE
BLOCK=4096

[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
[ -n "${SUDO_USER:-}" ] || { echo "run via sudo from the target user" >&2; exit 1; }
[ "$(sysctl -n hw.model)" = MacBookPro18,1 ] || { echo "unexpected Mac model" >&2; exit 1; }
HOME_DIR=$(dscl . -read "/Users/$SUDO_USER" NFSHomeDirectory | awk '{print $2}')
SSH_DIR="$HOME_DIR/.ssh"
[ -f "$SSH_DIR/id_ed25519" ] && [ -f "$SSH_DIR/id_ed25519.pub" ] || { echo "no id_ed25519 in $SSH_DIR" >&2; exit 1; }

PLIST=$(diskutil info -plist "$DISK_UUID")
ID=$(echo "$PLIST" | plutil -extract DeviceIdentifier raw -o - -)
INTERNAL=$(echo "$PLIST" | plutil -extract Internal raw -o - -)
TOTAL=$(echo "$PLIST" | plutil -extract TotalSize raw -o - -)
[ "$INTERNAL" = true ] && [ "$TOTAL" = 79865782272 ] || { echo "target is not the expected private Windows container" >&2; exit 1; }
case "$ID" in disk0s[0-9]*) ;; *) echo "unexpected device $ID" >&2; exit 1 ;; esac
MOUNTED=$(echo "$PLIST" | plutil -extract Mounted raw -o - - 2>/dev/null || echo false)
MOUNTPOINT=$(echo "$PLIST" | plutil -extract MountPoint raw -o - - 2>/dev/null || true)
[ "$MOUNTED" = false ] && [ -z "$MOUNTPOINT" ] || { echo "container is mounted" >&2; exit 1; }

OFFSET=$(python3 -I - "$LAYOUT" "$BLOCK" <<'PY'
import json, sys
layout = json.load(open(sys.argv[1]))
part = [p for p in layout["partitions"] if p["name"] == "J316SOURCE"][0]
print(part["first"] * int(sys.argv[2]))
PY
)
TARGET="/dev/$ID@@$OFFSET"
# The partition must carry the installer marker (disk GUID) created by tools/j316s_install/layout.py.
GUID=$(python3 -I -c "import json,sys;print(json.load(open(sys.argv[1]))['disk_guid'])" "$LAYOUT")
[ "$(mcopy -i "$TARGET" ::/J316-INSTALL.SOURCE - | tr -d '\r\n')" = "$GUID" ] || { echo "J316SOURCE marker mismatch; nothing written" >&2; exit 1; }

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
chmod 700 "$STAGE"
cp "$SSH_DIR/id_ed25519" "$SSH_DIR/id_ed25519.pub" "$STAGE/"
printf 'Host github.com\n    IdentityFile ~/.ssh/id_ed25519\n    IdentitiesOnly yes\n' > "$STAGE/config"
ssh-keygen -F github.com -f "$SSH_DIR/known_hosts" | grep -v '^#' > "$STAGE/known_hosts"
[ -s "$STAGE/known_hosts" ] || { echo "no github.com host keys in known_hosts" >&2; exit 1; }
cp "$ROOT/tools/j316s_install/install-ssh.cmd" "$STAGE/"
# Windows batch needs CRLF.
sed -i '' 's/$/\r/' "$STAGE/install-ssh.cmd"

mmd -i "$TARGET" ::/J316Ssh 2>/dev/null || true
for f in id_ed25519 id_ed25519.pub config known_hosts install-ssh.cmd; do
    mcopy -o -i "$TARGET" "$STAGE/$f" "::/J316Ssh/$f"
done
sync
for f in id_ed25519 id_ed25519.pub config known_hosts install-ssh.cmd; do
    mcopy -i "$TARGET" "::/J316Ssh/$f" - | cmp - "$STAGE/$f" || { echo "READBACK MISMATCH: $f" >&2; exit 1; }
done
echo "OK: staged on J316SOURCE ::/J316Ssh (readback verified): $(ssh-keygen -lf "$SSH_DIR/id_ed25519.pub")"
echo "In Windows: open the J316SOURCE drive and run J316Ssh\\install-ssh.cmd"
