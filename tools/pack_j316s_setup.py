#!/usr/bin/env python3
"""Validate the isolated J316s firmware and assemble a hardware-unqualified Setup candidate."""

import argparse
from collections import Counter
from pathlib import Path
import re
import shutil
import struct
import subprocess
from tempfile import TemporaryDirectory
import zlib


ROOT = Path(__file__).resolve().parents[1]
PLATFORM = ROOT / "mu/Platform/J316sSetupPkg"
HEADER = struct.Struct("<8s4I")
AFFINITIES = (0, 1, 0x10100, 0x10101, 0x10102, 0x10103, 0x10200, 0x10201, 0x10202, 0x10203)
REQUIRED_MODULES = {
    "RamDiskDxe", "BootRamdiskHelperDxe", "WindowsAutoBootDxe", "SimpleFbDxe",
    "ArmGicDxe", "J316sCpuTables", "J316sDeviceTables",
}
FORBIDDEN_MODULES = re.compile(r"usb|xhci|ehci|uhci|nvme|nand|ans2|dart|pci|gpio|dfci", re.I)


def setup_constants() -> dict[str, int]:
    source = (ROOT / "m1n1_windows/src/hv_j316s_setup.h").read_text()
    return {key: int(value, 16) for key, value in re.findall(
        r"^#define J316S_SETUP_(\w+) (0x[\da-f]+)ULL$", source, re.M | re.I
    )}


def verify_layout(layout: dict[str, int]) -> None:
    regions = sorted([
        (layout["ADT"], layout["ADT_MAX"], "ADT"),
        (layout["FIRMWARE"], layout["FIRMWARE_SIZE"], "firmware"),
        (layout["BOOT_ARGS"], 0x4000, "boot arguments"),
        (layout["FB"], 1280 * 720 * 4, "framebuffer"),
        (layout["LOW_BACKING"], layout["LOW_SIZE"], "low memory backing"),
        (layout["RAMDISK"], layout["RAMDISK_SIZE"], "RAM disk"),
    ])
    end = layout["RAM_BASE"]
    for start, size, name in regions:
        if start < end or start + size > layout["RAM_BASE"] + layout["RAM_SIZE"]:
            raise ValueError(f"{name} overlaps another region or lies outside guest RAM")
        end = start + size

    dsc = (PLATFORM / "J316sSetup.dsc").read_text()
    pcds = {
        "PcdSystemMemoryBase": "RAM_BASE", "PcdSystemMemorySize": "RAM_SIZE",
        "PcdBootArgsPointer": "BOOT_ARGS", "PcdAdtPointer": "ADT",
        "PcdPreloadedRamdiskBase": "RAMDISK", "PcdPreloadedRamdiskMaxSize": "RAMDISK_SIZE",
        "PcdLowMemoryWindowBase": "LOW_IPA", "PcdLowMemoryWindowSize": "LOW_SIZE",
        "PcdLowMemoryWindowBackingBase": "LOW_BACKING", "PcdAppleUartBase": "UART",
        "PcdGicDistributorBase": "GIC_DIST", "PcdGicRedistributorsBase": "GIC_REDIST",
    }
    for pcd, field in pcds.items():
        match = re.search(rf"\.{pcd}\|(0x[\da-f]+)\s*$", dsc, re.M | re.I)
        if not match or int(match[1], 16) != layout[field]:
            raise ValueError(f"Mu {pcd} differs from m1n1 {field}")
    fdf = (PLATFORM / "J316sSetup.fdf").read_text()
    for name, field in (("BaseAddress", "FIRMWARE"), ("Size", "FIRMWARE_SIZE")):
        match = re.search(rf"^{name}\s*=\s*(0x[\da-f]+)\|", fdf, re.M | re.I)
        if not match or int(match[1], 16) != layout[field]:
            raise ValueError(f"Mu FD {name} differs from m1n1 {field}")


def verify_madt(data: bytes, layout: dict[str, int]) -> None:
    if len(data) < 44 or data[:4] != b"APIC" or struct.unpack_from("<I", data, 4)[0] != len(data):
        raise ValueError("invalid MADT header")
    cpu_entries = []
    entry_types = Counter()
    offset = 44
    while offset < len(data):
        if len(data) - offset < 2:
            raise ValueError("truncated MADT entry")
        kind, length = data[offset:offset + 2]
        if length != {11: 80, 12: 24, 14: 16}.get(kind) or offset + length > len(data):
            raise ValueError(f"unexpected MADT entry type or length ({kind}, {length})")
        entry = data[offset:offset + length]
        entry_types[kind] += 1
        if kind == 11:
            uid, flags = struct.unpack_from("<II", entry, 8)
            mpidr, = struct.unpack_from("<Q", entry, 68)
            cpu_entries.append((uid, mpidr, flags, entry[76]))
        elif kind == 12:
            if struct.unpack_from("<Q", entry, 8)[0] != layout["GIC_DIST"] or entry[20] != 3:
                raise ValueError("compiled MADT has the wrong GIC distributor")
        elif struct.unpack_from("<QI", entry, 4) != (layout["GIC_REDIST"], 10 * 0x20000):
            raise ValueError("compiled MADT has the wrong redistributor base or length")
        offset += length

    expected = [(uid, mpidr, 1, int(uid >= 2)) for uid, mpidr in enumerate(AFFINITIES)]
    if cpu_entries != expected or entry_types != {11: 10, 12: 1, 14: 1}:
        raise ValueError("compiled MADT differs from the unbinned T6000 CPU topology")


def verify_firmware(build: Path, layout: dict[str, int]) -> Path:
    firmware = build / "FV/J316S_SETUP_EFI.fd"
    if firmware.stat().st_size != layout["FIRMWARE_SIZE"]:
        raise ValueError("firmware size differs from m1n1")
    with firmware.open("rb") as stream:
        stream.seek(0x38)
        if stream.read(4) != b"ARMd":
            raise ValueError("firmware lacks its ARM64 entry header")

    madt = build / "AARCH64/J316sSetupPkg/AcpiTables/J316sCpuTables/OUTPUT/MADT.acpi"
    verify_madt(madt.read_bytes(), layout)
    volume = (build / "FV/FVMAIN.inf").read_text()
    modules = set(re.findall(r"/Ffs/[0-9a-f-]{36}([^/\n]+)/", volume, re.I))
    forbidden = {module for module in modules if FORBIDDEN_MODULES.search(module)}
    missing = REQUIRED_MODULES - modules
    if missing or forbidden:
        raise ValueError(f"unexpected firmware modules (missing={sorted(missing)}, forbidden={sorted(forbidden)})")
    return firmware


def file_crc32(path: Path) -> int:
    checksum = 0
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            checksum = zlib.crc32(chunk, checksum)
    return checksum


def pack(loader: Path, firmware: Path, media: Path, output: Path, layout: dict[str, int]) -> None:
    media_size = media.stat().st_size
    if (media_size % 512 or
            not 1024 * 1024 <= media_size <= layout["RAMDISK_SIZE"] - layout["RAMDISK_HEADER_SIZE"]):
        raise ValueError("RAM disk is outside the permitted size")
    with media.open("rb") as stream:
        sector = stream.read(512)
    if sector[82:90] != b"FAT32   " or sector[510:512] != b"\x55\xaa":
        raise ValueError("RAM disk lacks a FAT32 boot sector")

    symbols = {}
    for line in subprocess.check_output(["llvm-nm", "--format=posix", str(loader)], text=True).splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[0] in {"_base", "_payload_start", "hv_j316s_setup_run"}:
            symbols[parts[0]] = int(parts[2], 16)
    if len(symbols) != 3:
        raise ValueError("loader ELF was not built with J316S_SETUP=1")
    payload_offset = symbols["_payload_start"] - symbols["_base"]

    with TemporaryDirectory(prefix=".j316s-pack-", dir=output.parent) as directory:
        scratch = Path(directory)
        raw = scratch / "m1n1.bin"
        subprocess.run(["llvm-objcopy", "-O", "binary", "--strip-debug", str(loader), str(raw)], check=True)
        if raw.stat().st_size != payload_offset or payload_offset % 0x4000:
            raise ValueError("raw m1n1 size does not match its payload address")

        header = HEADER.pack(b"ASIJ316S", media_size, file_crc32(firmware), file_crc32(media), 0)
        candidate = scratch / "j316s-setup.bin"
        with candidate.open("wb") as stream:
            with raw.open("rb") as source:
                shutil.copyfileobj(source, stream)
            stream.write(header.ljust(layout["PAYLOAD_OFFSET"], b"\0"))
            for part in (firmware, media):
                with part.open("rb") as source:
                    shutil.copyfileobj(source, stream)
        output.hardlink_to(candidate)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mu-build", type=Path, required=True)
    parser.add_argument("--loader-elf", type=Path)
    parser.add_argument("--media", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if any((args.loader_elf, args.media, args.output)) and not all((args.loader_elf, args.media, args.output)):
        parser.error("packing requires --loader-elf, --media, and --output")
    try:
        layout = setup_constants()
        verify_layout(layout)
        firmware = verify_firmware(args.mu_build, layout)
        print(f"Verified isolated Mu firmware and ten-core MADT: {firmware}")
        if args.output:
            pack(args.loader_elf, firmware, args.media, args.output, layout)
            print(f"Wrote hardware-unqualified Setup candidate: {args.output}")
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
