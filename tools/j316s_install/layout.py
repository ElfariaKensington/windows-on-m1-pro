"""Build the private 4Kn installation disk; never open a physical device."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import uuid
import zlib

ROOT = Path(__file__).resolve().parents[2]
BLOCK = 4096
MIB = 1024 * 1024
DISK_BYTES = 80_000_000_000
ESP_BYTES = 300 * MIB
MSR_BYTES = 16 * MIB
SOURCE_BYTES = 9 * 1024 * MIB
INSTALL_TYPE = re.search(r'HV_NVME_J316S_INSTALL_GUID "([^"]+)"',
    (ROOT / "m1n1_windows/src/hv_nvme_gpt.h").read_text())[1]
EFI = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
MSR = "E3C9E316-0B5C-4DB8-817D-F92DF00215AE"
DATA = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"


def digest(path):
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def geometry(size=DISK_BYTES):
    if size % BLOCK or size < 64 * 1024**3:
        raise ValueError("Installation disk must be aligned and at least 64 GiB")
    blocks = size // BLOCK
    source_first = ((blocks - 6 - SOURCE_BYTES // BLOCK) // 256) * 256
    parts = [dict(number=1, type=EFI, first=256, blocks=ESP_BYTES // BLOCK, name="System"),
             dict(number=2, type=MSR, first=256 + ESP_BYTES // BLOCK,
                  blocks=MSR_BYTES // BLOCK, name="Microsoft reserved"),
             dict(number=3, type=DATA, first=source_first,
                  blocks=SOURCE_BYTES // BLOCK, name="J316SOURCE")]
    free_first = parts[1]["first"] + parts[1]["blocks"]
    if (source_first - free_first) * BLOCK < 64_000_000_000:
        raise ValueError("Insufficient Windows target space after reserving source media")
    return dict(bytes=size, block_size=BLOCK, disk_guid=str(uuid.uuid4()).upper(),
                outer_type=INSTALL_TYPE, partitions=parts,
                windows_unallocated=dict(first=free_first, blocks=source_first-free_first))


def write_gpt(path, layout):
    if path.exists() or path.is_symlink():
        raise FileExistsError(path)
    if str(path).startswith("/dev/"):
        raise ValueError("Only a new regular image file is allowed")
    size = layout["bytes"]; count = size // BLOCK
    table = bytearray(16384)
    for p in layout["partitions"]:
        struct.pack_into("<16s16sQQQ72s", table, (p["number"] - 1) * 128,
            uuid.UUID(p["type"]).bytes_le, uuid.uuid4().bytes_le, p["first"],
            p["first"] + p["blocks"] - 1, 0, p["name"].encode("utf-16le"))
    def header(current, alternate, location):
        h = bytearray(BLOCK)
        struct.pack_into("<8sIIIIQQQQ16sQIII", h, 0, b"EFI PART", 0x10000, 92, 0, 0,
            current, alternate, 6, count-6, uuid.UUID(layout["disk_guid"]).bytes_le,
            location, 128, 128, zlib.crc32(table))
        struct.pack_into("<I", h, 16, zlib.crc32(h[:92])); return h
    with path.open("xb") as f:
        f.truncate(size)
        mbr = bytearray(BLOCK); mbr[450] = 0xee
        struct.pack_into("<II", mbr, 454, 1, min(count-1, 0xffffffff))
        mbr[510:512] = b"\x55\xaa"; f.write(mbr)
        f.seek(BLOCK); f.write(header(1, count-1, 2)); f.write(table)
        f.seek((count-5)*BLOCK); f.write(table); f.write(header(count-1, 1, count-5))


def build(source, package, output):
    layout = geometry(); write_gpt(output, layout)
    source_part = layout["partitions"][2]
    for p, label in [(layout["partitions"][0], "J316SYSTEM"), (source_part, "J316SOURCE")]:
        subprocess.run(["mformat", "-i", f'{output}@@{p["first"]*BLOCK}',
            "-S", "5", "-T", str(p["blocks"]), "-F", "-v", label, "::"], check=True)
    target = f'{output}@@{source_part["first"]*BLOCK}'
    # Installation files only: no fallback EFI loader that could boot an unserviced PE.
    subprocess.run(["mmd", "-i", target, "::/sources", "::/J316Drivers",
        "::/sources/$OEM$", "::/sources/$OEM$/$1", "::/sources/$OEM$/$1/J316Input"], check=True)
    inputs = [p for p in (source / "sources").iterdir() if p.name != "boot.wim"]
    subprocess.run(["mcopy", "-s", "-i", target, *map(str, inputs), "::/sources/"], check=True)
    files = ["J316SpiInput.sys", "J316SpiInput.inf", "J316VhfInput.inf",
             "J316SpiInput.cat", "J316SpiInput.cer", "J316Tools.exe"]
    for directory in ["::/J316Drivers/", "::/sources/$OEM$/$1/J316Input/"]:
        subprocess.run(["mcopy", "-i", target, *[str(package/n) for n in files], directory], check=True)
    prepare = ROOT / "tools/j316s_install/prepare-input.cmd"
    subprocess.run(["mcopy", "-i", target, str(prepare),
        "::/sources/$OEM$/$1/J316Input/prepare-input.cmd"], check=True)
    marker = output.with_suffix(".marker.txt"); marker.write_text(layout["disk_guid"] + "\n")
    subprocess.run(["mcopy", "-i", target, str(marker), "::/J316-INSTALL.SOURCE"], check=True)
    layout["source_files"] = {n:digest(source/"sources"/n) for n in ["install.swm","install2.swm"]}
    layout["package"] = {n:digest(package/n) for n in files}
    output.with_suffix(".layout.json").write_text(json.dumps(layout, indent=2) + "\n")
    return layout


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    for key in ("source", "package", "output"): p.add_argument("--"+key, type=Path, required=True)
    a=p.parse_args(); print(json.dumps(build(a.source, a.package, a.output), indent=2))
