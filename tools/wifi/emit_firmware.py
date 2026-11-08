#!/usr/bin/env python3
"""Embed only the locally hash-pinned bundle; never write it into the source tree."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LOCAL = ROOT / ".build/j316s-wifi/local"


def main():
    manifest = json.loads((LOCAL / "manifest.json").read_text())
    if (manifest["model"], manifest["chip"], manifest["board"]) != (
            "MacBookPro18,1", "BCM4387C2", "madagascar"):
        raise ValueError("Firmware belongs to another board")
    roles = {"firmware": "firmware.bin", "nvram": "nvram.txt", "clm": "clm_blob",
             "txcap": "txcap_blob", "calibration": "calibration.bin", "signature": "firmware.sig"}
    lines = ['/* Private generated file; proprietary firmware. Do not redistribute. */']
    initializers = []
    for role, name in roles.items():
        item = manifest["files"].get(name)
        if item is None:
            initializers.append(f".{role} = {{0, 0}}")
            continue
        data = (LOCAL / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != item["sha256"]:
            raise ValueError(f"Local firmware hash changed: {name}")
        symbol = "j316_fw_" + role
        lines.append(f"static const u8 {symbol}[] = {{")
        lines.extend(",".join(str(x) for x in data[i:i+32]) + "," for i in range(0, len(data), 32))
        lines.append("};")
        initializers.append(f".{role} = {{{symbol}, sizeof({symbol})}}")
    mac = (LOCAL / "station-mac.bin").read_bytes()
    if len(mac) != 6 or mac[0] & 1 or mac == bytes(6):
        raise ValueError("Invalid station MAC")
    lines.append("static const u8 j316_wifi_mac[6] = {" + ",".join(map(str, mac)) + "};")
    lines.append("static const struct bcm_fw_bundle j316_wifi_firmware = {" +
                 ",".join(initializers) + "};")
    target = LOCAL / "wifi_firmware.h"
    target.write_text("\n".join(lines) + "\n")
    target.chmod(0o600)
    print("Emitted private, hash-checked firmware header.")


if __name__ == "__main__":
    main()
