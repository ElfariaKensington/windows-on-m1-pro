#!/usr/bin/env python3
"""Collect J316s Wi-Fi facts and the owner's exact firmware without device writes.

Protocol references (not copied implementations): Asahi asahi_firmware/wifi.py,
brcmfmac/pcie.c:brcmf_pcie_parse_otp_sys_vendor, and kboot.c:dt_set_wifi.
This prepares input for bring-up; it does not install a Windows Wi-Fi driver.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
LOCAL = ROOT / ".build/j316s-wifi"


def nodes(tree, parent=""):
    if isinstance(tree, list):
        for child in tree:
            yield from nodes(child, parent)
        return
    path = parent + "/" + tree.get("IORegistryEntryName", "")
    yield path, tree
    for child in tree.get("IORegistryEntryChildren", []):
        yield from nodes(child, path)


def board_parameters(otp):
    """Recover only module/vendor/revision; never retain serial-bearing OTP."""
    candidates = set()
    for text in re.findall(rb"[\x20-\x7e]{6,}", otp):
        fields = {}
        for field in text.decode("ascii").split():
            if "=" not in field:
                continue
            key, value = field.split("=", 1)
            if key in ("M", "V", "m"):
                if key in fields or not re.fullmatch(r"[A-Za-z0-9.]{1,15}", value):
                    raise ValueError("Malformed or duplicate OTP board parameter")
                fields[key] = value
        if set(fields) == {"M", "V", "m"}:
            candidates.add(tuple(fields[key] for key in ("M", "V", "m")))
    if len(candidates) != 1:
        raise ValueError("OTP does not identify exactly one module/vendor/revision")
    return next(iter(candidates))


def first_file(directory, names, optional=False):
    """Follow Apple's real board aliases; never guess a different board blob."""
    directory = directory.resolve()
    for name in names:
        candidate = directory / name
        if candidate.is_file():
            resolved = candidate.resolve()
            if not resolved.is_relative_to(directory):
                raise ValueError("Firmware symlink escapes chip directory")
            return candidate
    if optional:
        return None
    raise FileNotFoundError("No board-specific firmware: " + ", ".join(names))


def private_file(path, data):
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    temporary = path.with_name(path.name + ".tmp")
    fd = os.open(temporary, os.O_CREAT | os.O_TRUNC | os.O_WRONLY, 0o600)
    with os.fdopen(fd, "wb") as stream:
        stream.write(data)
    os.replace(temporary, path)
    path.chmod(0o600)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=LOCAL / "local")
    args = parser.parse_args()
    output = args.output.resolve()
    if not output.is_relative_to(LOCAL.resolve()):
        parser.error("Private firmware must stay under .build/j316s-wifi")
    model = subprocess.check_output(["sysctl", "-n", "hw.model"], text=True).strip()
    if model != "MacBookPro18,1":
        parser.error("This collector is restricted to MacBookPro18,1")
    tree = plistlib.loads(subprocess.check_output(["ioreg", "-a", "-l", "-p", "IODeviceTree"]))
    entries = list(nodes(tree))
    wlan = next(n for path, n in entries if path.endswith("/arm-io/wlan"))
    if wlan.get("module-instance") != b"madagascar\0":
        raise ValueError("Unexpected board type")
    antenna = wlan["wifi-antenna-sku-info"][8:16].split(b"\0", 1)[0].decode("ascii")
    if not re.fullmatch(r"[A-Za-z0-9]{1,8}", antenna):
        raise ValueError("Invalid antenna SKU")
    service = plistlib.loads(subprocess.check_output(
        ["ioreg", "-a", "-r", "-c", "AppleBCMWLANBusInterfacePCIe"]))
    chips = [n for _, n in nodes(service) if "ChipOTP" in n]
    if len(chips) != 1:
        raise ValueError("Expected one live Wi-Fi device")
    module, vendor, revision = board_parameters(chips[0]["ChipOTP"])
    chip_dir = Path("/usr/share/firmware/wifi/C-4387__s-C2")
    boards = [f"madagascar-{antenna}", "madagascar"]
    nvram_names = [f"P-{board}_M-{module}_V-{vendor}__m-{revision}.txt" for board in boards]
    files = {
        "firmware.bin": first_file(chip_dir, [board + ".trx" for board in boards]),
        "nvram.txt": first_file(chip_dir, nvram_names),
        "clm_blob": first_file(chip_dir, [board + ".clmb" for board in boards]),
        "txcap_blob": first_file(chip_dir, [board + ".txcb" for board in boards]),
        "firmware.sig": first_file(chip_dir, [board + ".sig" for board in boards], optional=True),
    }
    manifest = {"schema": 1, "model": model, "chip": "BCM4387C2", "board": "madagascar",
                "antenna": antenna, "module": module, "vendor": vendor, "revision": revision,
                "macos": subprocess.check_output(["sw_vers", "-buildVersion"], text=True).strip(),
                "files": {}, "hardware": {}, "windows_wifi": "not implemented"}
    for name, source in files.items():
        if source is None:
            manifest["files"][name] = None
            continue
        data = source.read_bytes()
        private_file(output / name, data)
        manifest["files"][name] = {"source": str(source), "resolved": str(source.resolve()),
                                   "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    calibration = wlan["wifi-calibration-msf"]
    private_file(output / "calibration.bin", calibration)
    mac = wlan["local-mac-address"]
    if len(mac) != 6 or mac[0] & 1 or mac == bytes(6):
        raise ValueError("Invalid station MAC")
    private_file(output / "station-mac.bin", mac)
    allowed = {"reg", "compatible", "interrupts", "function-perst", "function-clkreq",
               "function-dart_self", "AAPL,phandle"}
    for path, node in entries:
        if path.endswith(("/arm-io/apcie", "/arm-io/apcie/pci-bridge0", "/arm-io/dart-apcie0")):
            manifest["hardware"][path] = {key: value.hex() if isinstance(value, bytes) else value
                                          for key, value in node.items() if key in allowed}
    manifest["files"]["calibration.bin"] = {"bytes": len(calibration),
        "sha256": hashlib.sha256(calibration).hexdigest()}
    private_file(output / "manifest.json", (json.dumps(manifest, indent=2) + "\n").encode())
    print(f"Prepared exact BCM4387C2 madagascar/{antenna}/{module}/{vendor}/{revision} firmware: {output}")
    print("Read-only collection; Windows Wi-Fi and boot image unchanged.")


if __name__ == "__main__":
    main()
