#!/usr/bin/env python3
"""Build a fresh J316s Setup WIM and hash-manifested boot payload."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from build_j316s_setup_media import build_wimboot_media


def digest(path):
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("package", "base-wim", "setup-source", "bcd", "wimboot", "output"):
        parser.add_argument("--" + option, type=Path, required=True)
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    manifest = args.output.with_suffix(".manifest.json")
    if args.output.exists() or manifest.exists():
        parser.error("Output or manifest already exists")
    files = ("J316SpiInput.sys", "J316SpiInput.inf", "J316VhfInput.inf", "J316SpiInput.cat",
             "J316SpiInput.cer", "J316Tools.exe")
    for name in files:
        if not (args.package / name).is_file():
            parser.error(f"Missing signed package file: {name}")
    sources = ROOT / "drivers/j316-spi-input"
    with tempfile.TemporaryDirectory(prefix="j316-fresh-", dir=args.output.parent) as tmp:
        scratch = Path(tmp)
        wim = scratch / "boot.wim"
        shutil.copyfile(args.base_wim, wim)
        ini = scratch / "winpeshl.ini"
        ini.write_bytes(b"[LaunchApps]\r\n%SYSTEMROOT%\\System32\\cmd.exe, /c X:\\J316Input\\start.cmd\r\n")
        commands = [f'add "{ini}" /Windows/System32/winpeshl.ini']
        for name in files:
            commands.append(f'add "{(args.package / name).resolve()}" /J316Input/{name}')
        for name in ("start.cmd", "logs.cmd"):
            path = scratch / name
            path.write_bytes((sources / name).read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
            commands.append(f'add "{path}" /J316Input/{name}')
        subprocess.run(["wimlib-imagex", "update", str(wim), "2"],
                       input="\n".join(commands) + "\n", text=True, check=True)
        subprocess.run(["wimlib-imagex", "verify", str(wim)], check=True)
        media = scratch / "setup.img"
        build_wimboot_media(args.setup_source, args.wimboot, media, boot_wim=wim, bcd=args.bcd)
        subprocess.run([sys.executable, str(ROOT / "tools/pack_j316s_setup.py"),
                        "--mu-build", str(ROOT / "mu/Build/J316sSetup-AARCH64/DEBUG_CLANGPDB"),
                        "--loader-elf", str(ROOT / "m1n1_windows/build/m1n1-raw.elf"),
                        "--media", str(media), "--output", str(args.output),
                        "--usb", "--apple-input", "--diagnostics-slice"], check=True)
        data = {
            "experiment": "J316S-FRESH-SPI-001",
            "profile": "J316s Setup: independent SPI keyboard and relative touchpad",
            "native_input_test": "pending user test",
            "candidate": {"path": str(args.output), "sha256": digest(args.output)},
            "package": {name: digest(args.package / name) for name in files},
            "startup_scripts": {name: digest(scratch / name) for name in ("start.cmd", "logs.cmd")},
            "base_wim_sha256": digest(args.base_wim), "boot_wim_sha256": digest(wim),
            "bcd_sha256": digest(args.bcd), "media_sha256": digest(media),
            "mu_sha256": digest(ROOT / "mu/Build/J316sSetup-AARCH64/DEBUG_CLANGPDB/FV/J316S_SETUP_EFI.fd"),
            "m1n1_sha256": digest(ROOT / "m1n1_windows/build/m1n1-raw.elf"),
            "payload_flags": "0x0b",
        }
        manifest.write_text(json.dumps(data, indent=2) + "\n")
    print(f"Built {args.output}\nSHA256 {data['candidate']['sha256']}")


if __name__ == "__main__":
    main()
