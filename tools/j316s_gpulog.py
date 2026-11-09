#!/usr/bin/env python3
"""J316s GPU probe log: allocate it in an ESP image, verify it, decode it.

prepare  IMG  -> adds ::m1n1/gpulog.bin (contiguous, magic+random token) and
                 writes the C header m1n1 embeds (partition, offset, token).
verify   IMG  -> re-checks that the file still sits at the header's offset.
decode   SRC  -> SRC is the ESP image, the raw ESP device, a mounted ESP
                 directory or a copied gpulog.bin; writes text/console/ADT.
"""

import argparse
import json
import os
import secrets
import struct
import subprocess
import sys
import uuid
from pathlib import Path

MAGIC = b"J316S-GPULOG-V1\0"
FILE_BYTES = 8 * 1024 * 1024
BLOCK = 4096
HDR_FMT = "<16s16sIIIIQQQQQQQQQQ32s96s"
HDR_FIELDS = ("magic token version header_bytes stage result_flags sequence ticks "
              "tick_hz text_off text_len console_off console_len adt_off adt_len "
              "adt_total build state").split()
FLAGS = ["LOG_OPEN", "ADT_SAVED", "ADT_TRUNCATED", "NODES_FOUND", "PMGR_READ",
         "POWER_ON_OK", "POWER_ON_FAIL", "MMIO_READ", "MMIO_FAULT", "POWER_RESTORED",
         "RESTORE_FAIL", "COMPLETE", "ALREADY_ACTIVE", "FW_SEG_READ", "FW_SEG_FAULT"]


class Fat32:
    def __init__(self, data: bytes):
        self.d = data
        bps, = struct.unpack_from("<H", data, 11)
        self.bps = bps
        self.spc = data[13]
        reserved, = struct.unpack_from("<H", data, 14)
        nfats = data[16]
        fatsz, = struct.unpack_from("<I", data, 36)
        self.root, = struct.unpack_from("<I", data, 44)
        if data[82:90] != b"FAT32   " or not bps or not self.spc or not fatsz:
            raise SystemExit("not a FAT32 image")
        self.fat_off = reserved * bps
        self.data_off = (reserved + nfats * fatsz) * bps
        self.csize = bps * self.spc

    def next(self, c):
        return struct.unpack_from("<I", self.d, self.fat_off + 4 * c)[0] & 0x0FFFFFFF

    def chain(self, c):
        out = []
        while 2 <= c < 0x0FFFFFF8:
            out.append(c)
            if len(out) > 1 << 22:
                raise SystemExit("FAT loop")
            c = self.next(c)
        return out

    def off(self, c):
        return self.data_off + (c - 2) * self.csize

    def entries(self, cluster):
        for c in self.chain(cluster):
            base = self.off(c)
            for i in range(0, self.csize, 32):
                e = self.d[base + i: base + i + 32]
                if e[0] == 0:
                    return
                if e[0] == 0xE5 or e[11] == 0x0F:
                    continue
                hi, = struct.unpack_from("<H", e, 20)
                lo, = struct.unpack_from("<H", e, 26)
                size, = struct.unpack_from("<I", e, 28)
                yield e[:11], e[11], (hi << 16) | lo, size

    def find(self, path):
        cluster = self.root
        parts = path.strip("/").split("/")
        for depth, name in enumerate(parts):
            stem, _, ext = name.upper().partition(".")
            short = stem.ljust(8).encode()[:8] + ext.ljust(3).encode()[:3]
            for n, attr, first, size in self.entries(cluster):
                if n == short:
                    if depth == len(parts) - 1:
                        return first, size
                    cluster = first
                    break
            else:
                return None
        return None


def extent(image: Path, path="m1n1/gpulog.bin"):
    data = image.read_bytes() if image.stat().st_size < (2 << 30) else None
    if data is None:
        raise SystemExit("image too large")
    fat = Fat32(data)
    found = fat.find(path)
    if not found:
        raise SystemExit(f"{path} not found in {image}")
    first, size = found
    chain = fat.chain(first)
    if size != FILE_BYTES or len(chain) * fat.csize < size:
        raise SystemExit(f"{path}: size {size} / {len(chain)} clusters unexpected")
    if any(b != a + 1 for a, b in zip(chain, chain[1:])):
        raise SystemExit(f"{path} is fragmented")
    offset = fat.off(first)
    if offset % BLOCK:
        raise SystemExit(f"{path} offset {offset:#x} is not {BLOCK}-aligned")
    return offset, data[offset: offset + 32]


def guid_bytes(text):
    return uuid.UUID(text).bytes_le


def c_bytes(b):
    return "{" + ", ".join(f"0x{x:02x}" for x in b) + "}"


def cmd_prepare(a):
    img = Path(a.image)
    token = secrets.token_bytes(16)
    blob = bytearray(FILE_BYTES)
    blob[:16] = MAGIC
    blob[16:32] = token
    tmp = Path(a.header).with_name("gpulog-initial.bin")
    tmp.parent.mkdir(parents=True, exist_ok=True)
    tmp.write_bytes(blob)
    subprocess.run(["mmd", "-D", "s", "-i", str(img), "::m1n1"], stderr=subprocess.DEVNULL)
    subprocess.run(["mcopy", "-D", "o", "-i", str(img), str(tmp), "::m1n1/gpulog.bin"],
                   check=True)
    offset, head = extent(img)
    if head != bytes(blob[:32]):
        raise SystemExit("magic/token not found at computed offset")
    if offset + FILE_BYTES > a.part_blocks * BLOCK:
        raise SystemExit("file beyond partition")
    hdr = f"""/* Generated by tools/j316s_gpulog.py prepare; do not edit. */
#ifndef J316S_GPULOG_LOCATION_H
#define J316S_GPULOG_LOCATION_H
#define J316S_GPULOG_BUILD "{a.build}"
#define J316S_GPULOG_PART_GUID {c_bytes(guid_bytes(a.part_uuid))} /* {a.part_uuid} */
#define J316S_GPULOG_PART_START_LBA {a.part_start_lba}ULL
#define J316S_GPULOG_PART_BLOCKS {a.part_blocks}ULL
#define J316S_GPULOG_FILE_OFFSET 0x{offset:x}ULL
#define J316S_GPULOG_FILE_BYTES 0x{FILE_BYTES:x}ULL
#define J316S_GPULOG_TOKEN {c_bytes(token)}
#endif
"""
    Path(a.header).write_text(hdr)
    meta = {"image": str(img), "file_offset": offset, "file_bytes": FILE_BYTES,
            "token": token.hex(), "part_uuid": a.part_uuid,
            "part_start_lba": a.part_start_lba, "part_blocks": a.part_blocks,
            "build": a.build}
    Path(a.header).with_suffix(".json").write_text(json.dumps(meta, indent=2) + "\n")
    print(f"gpulog.bin at ESP offset {offset:#x} (LBA {a.part_start_lba + offset // BLOCK:#x})")


def cmd_verify(a):
    meta = json.loads(Path(a.meta).read_text())
    offset, head = extent(Path(a.image))
    want = MAGIC + bytes.fromhex(meta["token"])
    if offset != meta["file_offset"] or head != want:
        raise SystemExit(f"gpulog moved or changed: offset {offset:#x} vs {meta['file_offset']:#x}")
    print(f"verified gpulog.bin at {offset:#x}")


def load_log(src: Path, meta):
    if src.is_dir():
        return (src / "m1n1" / "gpulog.bin").read_bytes()
    size = src.stat().st_size if src.is_file() else 0
    if src.is_file() and size == FILE_BYTES:
        return src.read_bytes()
    offset = meta["file_offset"] if meta else None
    if offset is None:
        raise SystemExit("need --meta to locate the log in a raw image/device")
    with open(src, "rb") as f:
        f.seek(offset)
        return f.read(FILE_BYTES)


def parse_adt(buf):
    out = []

    def node(off, path, depth):
        nprops, nchild = struct.unpack_from("<II", buf, off)
        off += 8
        props = {}
        for _ in range(nprops):
            name = buf[off:off + 32].split(b"\0")[0].decode(errors="replace")
            size, = struct.unpack_from("<I", buf, off + 32)
            size &= 0x7FFFFFFF
            props[name] = bytes(buf[off + 36: off + 36 + size])
            off += 36 + ((size + 3) & ~3)
        name = props.get("name", b"?").split(b"\0")[0].decode(errors="replace")
        full = path + "/" + name if depth else ""
        out.append((full or "/", props))
        for _ in range(nchild):
            off = node(off, full, depth + 1)
        return off

    node(0, "", 0)
    return out


def fmt_prop(v):
    if len(v) > 1 and v[-1] == 0 and all(32 <= c < 127 for c in v[:-1].rstrip(b"\0")):
        return '"' + v.rstrip(b"\0").decode() + '"'
    return v.hex()


def cmd_decode(a):
    meta = json.loads(Path(a.meta).read_text()) if a.meta else None
    raw = load_log(Path(a.source), meta)
    vals = dict(zip(HDR_FIELDS, struct.unpack_from(HDR_FMT, raw, 0)))
    if vals["magic"] != MAGIC:
        raise SystemExit("no J316s GPU log magic")
    if meta and vals["token"] != bytes.fromhex(meta["token"]):
        print("warning: token differs from this build's metadata", file=sys.stderr)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    flags = [n for i, n in enumerate(FLAGS) if vals["result_flags"] >> i & 1]
    summary = {k: (v.split(b"\0")[0].decode() if isinstance(v, bytes) and k in ("build", "state")
                   else v.hex() if isinstance(v, bytes) else v) for k, v in vals.items()}
    summary["flags"] = flags
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    text = raw[vals["text_off"]: vals["text_off"] + vals["text_len"]]
    console = raw[vals["console_off"]: vals["console_off"] + vals["console_len"]]
    adt = raw[vals["adt_off"]: vals["adt_off"] + vals["adt_len"]]
    (out / "probe.txt").write_bytes(text)
    (out / "console.txt").write_bytes(console)
    (out / "adt.bin").write_bytes(adt)
    if vals["sequence"] == 0:
        print("log never written by m1n1 (sequence 0): the probe did not run or could not persist")
    elif adt and vals["adt_len"] == vals["adt_total"]:
        nodes = parse_adt(adt)
        with open(out / "adt.txt", "w") as f:
            for path, props in nodes:
                f.write(path + "\n")
                for k, v in props.items():
                    f.write(f"  {k} [{len(v)}] {fmt_prop(v)}\n")
        gpu = {p: {k: fmt_prop(v) for k, v in props.items()} for p, props in nodes
               if p.startswith(("/arm-io/sgx", "/arm-io/gfx-asc"))
               or p in ("/chosen", "/chosen/carveout-memory-map")}
        (out / "adt-gpu.json").write_text(json.dumps(gpu, indent=2) + "\n")
    print(json.dumps({k: summary[k] for k in ("build", "stage", "state", "sequence", "flags",
                                             "text_len", "console_len", "adt_len", "adt_total")}))
    print(f"decoded into {out}")


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("prepare")
    s.add_argument("image")
    s.add_argument("--header", required=True)
    s.add_argument("--part-uuid", required=True)
    s.add_argument("--part-start-lba", type=int, required=True)
    s.add_argument("--part-blocks", type=int, required=True)
    s.add_argument("--build", required=True)
    s = sub.add_parser("verify")
    s.add_argument("image")
    s.add_argument("--meta", required=True)
    s = sub.add_parser("decode")
    s.add_argument("source")
    s.add_argument("--meta")
    s.add_argument("--out", required=True)
    a = p.parse_args()
    {"prepare": cmd_prepare, "verify": cmd_verify, "decode": cmd_decode}[a.cmd](a)


if __name__ == "__main__":
    main()
