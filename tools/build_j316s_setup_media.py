#!/usr/bin/env python3
"""Build a FAT32 RAM disk from an ARM64 Windows Setup ISO."""

import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
from tempfile import TemporaryDirectory

from j313_ramdisk import PAYLOAD_OFFSET

MIB = 1024 * 1024
RAMDISK_WINDOW_SIZE = 0x80000000
EFI_AARCH64 = 0xAA64


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


def build_media(iso: Path, output: Path, expected_sha256: str) -> None:
    with iso.open("rb") as source:
        digest = hashlib.file_digest(source, "sha256").hexdigest()
    if digest != expected_sha256.lower():
        raise ValueError(f"ISO SHA-256 mismatch (found {digest})")
    if output.exists():
        raise FileExistsError(output)

    with TemporaryDirectory(prefix=".j316s-media-", dir=output.parent) as directory:
        scratch = Path(directory)
        extracted = scratch / "contents"
        subprocess.run(
            ["7z", "x", "-y", f"-o{extracted}", str(iso), "boot", "efi", "sources/boot.wim"],
            check=True, stdout=subprocess.DEVNULL,
        )

        verify_efi_loader(extracted / "efi/boot/bootaa64.efi")
        for required in ("boot/boot.sdi", "efi/microsoft/boot/bcd"):
            if not (extracted / required).is_file():
                raise ValueError(f"ISO is missing {required}")
        with (extracted / "sources/boot.wim").open("rb") as wim:
            if wim.read(8) != b"MSWIM\0\0\0":
                raise ValueError("ISO has no Windows Setup WIM")

        source_size = sum(path.stat().st_size for path in extracted.rglob("*") if path.is_file())
        size = ((source_size + MIB - 1) // MIB + 64) * MIB
        if size > RAMDISK_WINDOW_SIZE - PAYLOAD_OFFSET:
            raise ValueError("Setup media exceeds the firmware RAM disk window")

        image = scratch / "winpe.img"
        with image.open("wb") as stream:
            stream.truncate(size)
        subprocess.run(["mformat", "-i", str(image), "-F", "-v", "WINPE", "::"], check=True)
        subprocess.run(
            ["mcopy", "-i", str(image), "-s", "boot", "efi", "sources", "::/"],
            cwd=extracted, check=True,
        )
        output.hardlink_to(image)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("iso", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--iso-sha256", required=True)
    args = parser.parse_args()
    try:
        build_media(args.iso.resolve(), args.output.resolve(), args.iso_sha256)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
