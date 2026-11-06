#!/usr/bin/env python3
"""Build the J316s WinPE RAM disk or the full ARM64 USB installation source."""

import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
from tempfile import TemporaryDirectory
import xml.etree.ElementTree as ET

from j313_ramdisk import PAYLOAD_OFFSET

MIB = 1024 * 1024
RAMDISK_WINDOW_SIZE = 0x80000000
EFI_AARCH64 = 0xAA64
WIMBOOT_SHA256 = "b1440c6386981fb447b2f341294ec7a59546d496dff0f075b0f698c853f1c949"


def verify_efi_loader(path: Path) -> None:
    data = path.read_bytes()
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise ValueError("BOOTAA64.EFI has no DOS header")
    pe_offset, = struct.unpack_from("<I", data, 0x3C)
    if pe_offset > len(data) - 6:
        raise ValueError("BOOTAA64.EFI has a truncated PE header")
    signature, machine = struct.unpack_from("<4sH", data, pe_offset)
    if signature != b"PE\0\0" or machine != EFI_AARCH64:
        raise ValueError("BOOTAA64.EFI is not an ARM64 EFI executable")


def build_wimboot_media(source: Path, wimboot: Path, output: Path,
                        boot_wim: Path | None = None, bcd: Path | None = None) -> None:
    if hashlib.sha256(wimboot.read_bytes()).hexdigest() != WIMBOOT_SHA256:
        raise ValueError("wimboot must match the official v2.9.0 ARM64 release")
    verify_efi_loader(wimboot)
    files = {
        "bootmgfw.efi": source / "efi/boot/bootaa64.efi",
        "BCD": bcd if bcd is not None else source / "efi/microsoft/boot/bcd",
        "boot.sdi": source / "boot/boot.sdi",
        "boot.wim": boot_wim if boot_wim is not None else source / "sources/boot.wim",
    }
    verify_efi_loader(files["bootmgfw.efi"])
    with files["boot.wim"].open("rb") as wim:
        if wim.read(8) != b"MSWIM\0\0\0":
            raise ValueError("Windows Setup WIM data is missing or invalid")

    source_size = wimboot.stat().st_size + sum(path.stat().st_size for path in files.values())
    size = ((source_size + MIB - 1) // MIB + 64) * MIB
    if size > RAMDISK_WINDOW_SIZE - PAYLOAD_OFFSET:
        raise ValueError("Setup media exceeds the firmware RAM disk window")
    if output.exists():
        raise FileExistsError(output)
    with TemporaryDirectory(prefix=".j316s-wimboot-", dir=output.parent) as directory:
        scratch = Path(directory)
        image = scratch / "winpe.img"
        with image.open("wb") as stream:
            stream.truncate(size)
        subprocess.run(["mformat", "-i", str(image), "-F", "-v", "WINPE", "::"], check=True)
        for name, path in {**files, "wimboot.efi": wimboot}.items():
            subprocess.run(["mcopy", "-i", str(image), str(path), f"::/{name}"], check=True)
        output.hardlink_to(image)


def verify_iso(iso: Path, expected_sha256: str) -> None:
    with iso.open("rb") as source:
        digest = hashlib.file_digest(source, "sha256").hexdigest()
    if digest != expected_sha256.lower():
        raise ValueError(f"ISO SHA-256 mismatch (found {digest})")


def build_usb_source(iso: Path, output: Path, expected_sha256: str) -> None:
    """Prepare the complete Setup source for an existing FAT32 USB volume."""
    verify_iso(iso, expected_sha256)
    if output.exists():
        raise FileExistsError(output)

    with TemporaryDirectory(prefix=".j316s-usb-source-", dir=output.parent) as directory:
        extracted = Path(directory) / "contents"
        subprocess.run(["7z", "x", "-y", f"-o{extracted}", str(iso)],
                       check=True, stdout=subprocess.DEVNULL)
        verify_efi_loader(extracted / "efi/boot/bootaa64.efi")
        sources = extracted / "sources"
        image = sources / "install.wim"
        if not (extracted / "setup.exe").is_file() or not (sources / "boot.wim").is_file():
            raise ValueError("ISO does not contain the Windows Setup files")
        info = subprocess.check_output(["wimlib-imagex", "info", str(image), "--xml"])
        editions = ET.fromstring(info).findall("IMAGE")
        if not editions or any(edition.findtext("WINDOWS/ARCH") != "12" for edition in editions):
            raise ValueError("Windows installation image does not contain ARM64 editions")

        if image.stat().st_size >= 4 * 1024 * MIB:
            split = sources / "install.swm"
            subprocess.run(["wimlib-imagex", "split", str(image), str(split), "3800"], check=True)
            subprocess.run(["wimlib-imagex", "verify", str(split),
                            f"--ref={sources / 'install*.swm'}"], check=True)
            image.unlink()
        else:
            subprocess.run(["wimlib-imagex", "verify", str(image)], check=True)

        for path in extracted.rglob("*"):
            if path.is_file() and path.stat().st_size >= 4 * 1024 * MIB:
                raise ValueError(f"{path.relative_to(extracted)} exceeds the FAT32 file limit")
        if output.exists():
            raise FileExistsError(output)
        extracted.rename(output)


def build_media(iso: Path, wimboot: Path, output: Path, expected_sha256: str,
                boot_wim: Path | None = None, bcd: Path | None = None) -> None:
    verify_iso(iso, expected_sha256)
    if output.exists():
        raise FileExistsError(output)

    with TemporaryDirectory(prefix=".j316s-media-", dir=output.parent) as directory:
        scratch = Path(directory)
        extracted = scratch / "contents"
        subprocess.run(
            ["7z", "x", "-y", f"-o{extracted}", str(iso), "boot", "efi", "sources/boot.wim"],
            check=True, stdout=subprocess.DEVNULL,
        )

        build_wimboot_media(extracted, wimboot, output, boot_wim, bcd)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("iso", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--iso-sha256", required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--wimboot", type=Path, help="Build the RAM disk using this wimboot EFI binary")
    mode.add_argument("--usb-source", action="store_true",
                      help="Extract full installation media into a new FAT32-compatible directory")
    parser.add_argument("--boot-wim", type=Path, help="Use a serviced ARM64 Setup WIM")
    parser.add_argument("--bcd", type=Path, help="Use a prepared Boot Configuration Data store")
    args = parser.parse_args()
    if args.usb_source and (args.boot_wim or args.bcd):
        parser.error("--boot-wim and --bcd apply only to the RAM disk")
    try:
        if args.usb_source:
            build_usb_source(args.iso.resolve(), args.output.resolve(), args.iso_sha256)
        else:
            build_media(args.iso.resolve(), args.wimboot.resolve(), args.output.resolve(),
                        args.iso_sha256, args.boot_wim, args.bcd)
    except (OSError, ValueError, ET.ParseError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
