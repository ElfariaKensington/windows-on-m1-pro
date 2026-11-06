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
    "RamDiskDxe", "BootRamdiskHelperDxe", "BdsDxe", "SimpleFbDxe",
    "ArmGicDxe", "J316sCpuTables", "J316sDeviceTables",
}
FORBIDDEN_MODULES = re.compile(r"usb|xhci|ehci|uhci|nvme|nand|ans2|dart|pci|gpio|dfci|WindowsAutoBootDxe", re.I)
VIRTUAL_STORAGE_MODULES = {"ArmPciCpuIo2Dxe", "PciHostBridgeDxe", "PciBusDxe", "NvmExpressDxe"}


def setup_constants() -> dict[str, int]:
    source = (ROOT / "m1n1_windows/src/hv_j316s_setup.h").read_text()
    return {key: int(value, 16) for key, value in re.findall(
        r"^#define J316S_SETUP_(\w+) (0x[\da-f]+)ULL$", source, re.M | re.I
    )}


def verify_layout(layout: dict[str, int]) -> None:
    # PrePi rounds the 32-bit framebuffer reservation up to 16 KiB.
    fb_size = (layout["FB_WIDTH"] * layout["FB_HEIGHT"] * 4 + 0x3fff) & ~0x3fff
    regions = sorted([
        (layout["ADT"], layout["ADT_MAX"], "ADT"),
        (layout["FIRMWARE"], layout["FIRMWARE_SIZE"], "firmware"),
        (layout["BOOT_ARGS"], 0x4000, "boot arguments"),
        (layout["FB"], fb_size, "framebuffer"),
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
        "PcdFrameBufferWidth": "FB_WIDTH", "PcdFrameBufferHeight": "FB_HEIGHT",
        "PcdVideoHorizontalResolution": "FB_WIDTH", "PcdVideoVerticalResolution": "FB_HEIGHT",
        "PcdSetupVideoHorizontalResolution": "FB_WIDTH", "PcdSetupVideoVerticalResolution": "FB_HEIGHT",
    }
    for pcd, field in pcds.items():
        match = re.search(rf"\.{pcd}\|(0x[\da-f]+|[0-9]+)\s*$", dsc, re.M | re.I)
        if not match or int(match[1], 0) != layout[field]:
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


def acpi_memory(base: int, size: int) -> bytes:
    return struct.pack("<BHBBB5Q", 0x8A, 43, 0, 0x0D, 1,
                       0, base, base + size - 1, 0, size)


def acpi_interrupt(irq: int) -> bytes:
    return struct.pack("<BHBBI", 0x89, 6, 1, 1, irq)


def verify_dsdt(dsdt: bytes, layout: dict[str, int], usb: bool, apple_input: bool,
                storage: bool = False) -> None:
    if len(dsdt) < 36 or dsdt[:4] != b"DSDT" or sum(dsdt) % 256:
        raise ValueError("invalid J316s DSDT")
    xhci_names = (b"XHC1", b"XHC2", b"XHC3")
    if tuple(name in dsdt for name in xhci_names) != (usb, usb, usb):
        raise ValueError("DSDT and requested USB profile differ")
    if usb:
        for index in range(3):
            name = f"XHC{index + 1}".encode()
            pnp0d15 = name + b"\x08_HID\x0c\x41\xd0\x0d\x15"
            resources = (acpi_memory(layout[f"XHCI{index}"], layout["XHCI_SIZE"]) +
                         acpi_interrupt(layout[f"XHCI_VINTID{index}"]))
            if pnp0d15 not in dsdt or resources not in dsdt:
                raise ValueError(f"{name.decode()} DSDT memory or interrupt differs from m1n1")
    if (b"AIP0" in dsdt) != apple_input:
        raise ValueError("DSDT and requested Apple input profile differ")
    if apple_input:
        memory = b"".join(acpi_memory(layout[name], 0x4000) for name in
                          ("INPUT_SPI", "INPUT_AP_GPIO", "INPUT_NUB_GPIO"))
        resources = memory + acpi_interrupt(layout["INPUT_VINTID"])
        if b"AIP0\x08_HID\x0dAPPL0001\0" not in dsdt or resources not in dsdt:
            raise ValueError("Apple input DSDT memory or interrupt differs from m1n1")
    if (b"PCI0" in dsdt) != storage:
        raise ValueError("DSDT and requested Windows disk profile differ")
    if storage:
        bar = bytearray(acpi_memory(layout["NVME_BAR"], layout["NVME_BAR_SIZE"]))
        bar[4] &= ~1  # ResourceProducer
        ecam = acpi_memory(layout["ECAM"], layout["ECAM_SIZE"])
        route = b"\x0a" + bytes([layout["NVME_INTID"]])
        if bar not in dsdt or ecam not in dsdt or b"_PRT" not in dsdt or route not in dsdt:
            raise ValueError("Windows disk DSDT resources differ from m1n1")


def verify_firmware(build: Path, layout: dict[str, int], usb: bool = False,
                    apple_input: bool = False, storage: bool = False) -> Path:
    firmware = build / "FV/J316S_SETUP_EFI.fd"
    if firmware.stat().st_size != layout["FIRMWARE_SIZE"]:
        raise ValueError("firmware size differs from m1n1")
    with firmware.open("rb") as stream:
        stream.seek(0x38)
        if stream.read(4) != b"ARMd":
            raise ValueError("firmware lacks its ARM64 entry header")

    madt = build / "AARCH64/J316sSetupPkg/AcpiTables/J316sCpuTables/OUTPUT/MADT.acpi"
    verify_madt(madt.read_bytes(), layout)
    table_module = "J316sStorageTables" if storage else "J316sDeviceTables"
    table_path = build / "AARCH64/J316sSetupPkg/AcpiTables" / table_module / "OUTPUT"
    verify_dsdt((table_path / "DSDT.aml").read_bytes(), layout, usb, apple_input, storage)
    if storage:
        mcfg = (table_path / "MCFG.acpi").read_bytes()
        if (len(mcfg) != 60 or mcfg[:4] != b"MCFG" or
                mcfg[44:] != struct.pack("<QHBBI", layout["ECAM"], 0, 0, 0, 0)):
            raise ValueError("Windows disk MCFG differs from m1n1")
        dsc = (PLATFORM / "J316sSetup.dsc").read_text()
        for name, field in (("PcdPciExpressBaseAddress", "ECAM"),
                            ("PcdPciMmio64Base", "NVME_BAR"),
                            ("PcdPciMmio64Size", "NVME_BAR_SIZE")):
            match = re.search(rf"\.{name}\|(0x[\da-f]+)\s*$", dsc, re.M | re.I)
            if not match or int(match[1], 16) != layout[field]:
                raise ValueError(f"Mu {name} differs from m1n1 {field}")
    volume = (build / "FV/FVMAIN.inf").read_text()
    modules = set(re.findall(r"/Ffs/[0-9a-f-]{36}([^/\n]+)/", volume, re.I))
    forbidden = {module for module in modules if FORBIDDEN_MODULES.search(module)}
    required = (REQUIRED_MODULES - {"J316sDeviceTables"}) | {table_module}
    if storage:
        required |= VIRTUAL_STORAGE_MODULES
        forbidden -= VIRTUAL_STORAGE_MODULES
    missing = required - modules
    alternate = "J316sDeviceTables" if storage else "J316sStorageTables"
    if alternate in modules:
        forbidden.add(alternate)
    if missing or forbidden:
        raise ValueError(f"unexpected firmware modules (missing={sorted(missing)}, forbidden={sorted(forbidden)})")
    return firmware


def file_crc32(path: Path) -> int:
    checksum = 0
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            checksum = zlib.crc32(chunk, checksum)
    return checksum


def pack(loader: Path, firmware: Path, media: Path, output: Path, layout: dict[str, int],
         usb: bool = False, apple_input: bool = False, storage: bool = False,
         diagnostics_slice: bool = False) -> None:
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
    if usb and "hv_j316s_setup_owns_usb" not in subprocess.check_output(
            ["llvm-nm", "--format=posix", str(loader)], text=True):
        raise ValueError("loader ELF lacks the J316s USB profile")
    payload_offset = symbols["_payload_start"] - symbols["_base"]

    with TemporaryDirectory(prefix=".j316s-pack-", dir=output.parent) as directory:
        scratch = Path(directory)
        raw = scratch / "m1n1.bin"
        subprocess.run(["llvm-objcopy", "-O", "binary", "--strip-debug", str(loader), str(raw)], check=True)
        if raw.stat().st_size != payload_offset or payload_offset % 0x4000:
            raise ValueError("raw m1n1 size does not match its payload address")

        flags = layout["USB_FLAG"] if usb else 0
        if apple_input:
            flags |= layout["INPUT_FLAG"]
        if diagnostics_slice:
            flags |= layout["DIAGNOSTICS_STORAGE_FLAG"]
        elif storage:
            flags |= layout["STORAGE_FLAG"]
        header = HEADER.pack(b"ASIJ316S", media_size, file_crc32(firmware), file_crc32(media), flags)
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
    parser.add_argument("--usb", action="store_true",
                        help="Enable all three J316s USB-C controllers for Setup")
    parser.add_argument("--apple-input", action="store_true",
                        help="Expose J316s SPI input resources for a prepared WinPE image")
    parser.add_argument("--windows-slice", action="store_true",
                        help="Use the explicitly reserved internal GPT partition as a disk")
    parser.add_argument("--diagnostics-slice", action="store_true",
                        help="Expose a bounded 1 GiB+ diagnostics-only internal GPT slice")
    args = parser.parse_args()
    if (args.apple_input or args.windows_slice or args.diagnostics_slice) and not args.usb:
        parser.error("Apple input and Windows storage profiles require --usb")
    if args.windows_slice and args.diagnostics_slice:
        parser.error("--windows-slice and --diagnostics-slice are mutually exclusive")
    if args.diagnostics_slice and not args.apple_input:
        parser.error("--diagnostics-slice requires --apple-input")
    if any((args.loader_elf, args.media, args.output)) and not all((args.loader_elf, args.media, args.output)):
        parser.error("packing requires --loader-elf, --media, and --output")
    try:
        layout = setup_constants()
        verify_layout(layout)
        storage = args.windows_slice or args.diagnostics_slice
        firmware = verify_firmware(args.mu_build, layout, args.usb, args.apple_input,
                                   storage)
        print(f"Verified Mu firmware, ten-core MADT and Setup DSDT: {firmware}")
        if args.output:
            pack(args.loader_elf, firmware, args.media, args.output, layout,
                 usb=args.usb, apple_input=args.apple_input, storage=args.windows_slice,
                 diagnostics_slice=args.diagnostics_slice)
            print(f"Wrote hardware-unqualified Setup candidate: {args.output}")
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
