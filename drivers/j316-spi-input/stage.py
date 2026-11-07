#!/usr/bin/env python3
"""Install/restore only the hash-pinned J316s EFI partition, without rebooting."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import plistlib
import subprocess

DISK = "disk0s4"
DEVICE = Path("/dev/rdisk0s4")
UUID = "9FBC60EC-E8FD-4270-8C54-B18BB7DDD4AE"
SIZE = 1000341504


def sha(path):
    h = hashlib.sha256()
    with path.open("rb", buffering=0) as f:
        remaining = SIZE if path == DEVICE else path.stat().st_size
        while remaining:
            block = f.read(min(4 * 1024 * 1024, remaining))
            if not block:
                raise RuntimeError(f"Short read: {path}")
            h.update(block)
            remaining -= len(block)
    return h.hexdigest()


def write_image(path):
    subprocess.run(["/bin/dd", f"if={path}", f"of={DEVICE}", "bs=4m"], check=True)
    subprocess.run(["/bin/sync"], check=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("manifest", type=Path)
    p.add_argument("--restore", action="store_true")
    args = p.parse_args()
    if os.geteuid() != 0:
        p.error("Run with administrator privileges")
    data = json.loads(args.manifest.read_text())
    info = plistlib.loads(subprocess.check_output(["/usr/sbin/diskutil", "info", "-plist", DISK]))
    model = subprocess.check_output(["/usr/sbin/sysctl", "-n", "hw.model"], text=True).strip()
    if (model != "MacBookPro18,1" or info.get("DiskUUID") != UUID or
            info.get("TotalSize") != SIZE or not info.get("Internal") or info.get("Content") != "EFI"):
        raise RuntimeError("Target identity changed; no write performed")
    if info.get("Mounted") or info.get("MountPoint"):
        raise RuntimeError("Target must be unmounted; no write performed")
    backup = Path(data["rollback"]["path"])
    image = Path(data["esp_image"]["path"])
    for path, expected in ((backup, data["rollback"]["sha256"]),
                           (image, data["esp_image"]["sha256"])):
        if path.stat().st_size != SIZE or sha(path) != expected:
            raise RuntimeError(f"Image or backup verification failed: {path}")
    expected_current = data["esp_image" if args.restore else "rollback"]["sha256"]
    if sha(DEVICE) != expected_current:
        raise RuntimeError("Installed partition changed; no write performed")
    selected = backup if args.restore else image
    wanted = data["rollback" if args.restore else "esp_image"]["sha256"]
    try:
        write_image(selected)
        actual = sha(DEVICE)
        if actual != wanted:
            raise RuntimeError("Full-partition readback mismatch")
    except Exception:
        if not args.restore:
            write_image(backup)
            if sha(DEVICE) != data["rollback"]["sha256"]:
                raise RuntimeError("Installation and automatic rollback both failed")
        raise
    receipt = args.manifest.with_suffix(".restore.json" if args.restore else ".installed.json")
    receipt.write_text(json.dumps({
        "time_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "device": str(DEVICE), "disk_uuid": UUID, "bytes": SIZE,
        "action": "restored" if args.restore else "installed",
        "readback_sha256": actual,
        "native_input_test": "pending user test",
    }, indent=2) + "\n")
    print(f"{'Restored' if args.restore else 'Installed'}; complete readback SHA256 {actual}")
    print(f"Receipt: {receipt}")


if __name__ == "__main__":
    main()
