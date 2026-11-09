"""Regression checks for the J316s GPU probe log: ESP allocation, header ABI, decode."""
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/j316s_gpulog.py"
PROBE = ROOT / "m1n1_windows/src/hv_j316s_gpu_probe.c"
sys.path.insert(0, str(ROOT / "tools"))
import j316s_gpulog as g  # noqa: E402

BLOCK = 4096


def run(*args):
    return subprocess.run([sys.executable, "-I", str(TOOL), *map(str, args)],
                          check=True, capture_output=True, text=True).stdout


def adt_node(props, children=()):
    out = struct.pack("<II", len(props), len(children))
    for name, value in props:
        out += name.encode().ljust(32, b"\0") + struct.pack("<I", len(value))
        out += value + b"\0" * (-len(value) % 4)
    for child in children:
        out += child
    return out


@unittest.skipUnless(shutil.which("mformat") and shutil.which("mcopy"), "mtools required")
class GpuLogTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.img = self.tmp / "esp.img"
        blocks = 16384  # 64 MiB, 4 KiB sectors like the J316s ESP
        with open(self.img, "wb") as f:
            f.truncate(blocks * BLOCK)
        subprocess.run(["mformat", "-i", str(self.img), "-F", "-S", "5", "-c", "1",
                        "-h", "16", "-s", "32", "-t", str(blocks // 512), "::"], check=True,
                       capture_output=True)
        subprocess.run(["mmd", "-i", str(self.img), "::m1n1"], check=True)
        (self.tmp / "boot.bin").write_bytes(b"x" * 300000)
        subprocess.run(["mcopy", "-i", str(self.img), str(self.tmp / "boot.bin"),
                        "::m1n1/boot.bin"], check=True)
        self.hdr = self.tmp / "local/j316s_gpulog_location.h"
        run("prepare", self.img, "--header", self.hdr, "--part-uuid",
            "9FBC60EC-E8FD-4270-8C54-B18BB7DDD4AE", "--part-start-lba", 1000,
            "--part-blocks", blocks, "--build", "J316s-GPU-TEST")
        self.meta = json.loads(self.hdr.with_suffix(".json").read_text())

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_header_abi_matches_c(self):
        self.assertEqual(struct.calcsize(g.HDR_FMT), 256)
        src = PROBE.read_text()
        self.assertIn("sizeof(struct gpulog_header) == 256", src)
        fields = re.search(r"struct gpulog_header \{(.*?)\} PACKED;", src, re.S).group(1)
        names = re.findall(r"\b(\w+)(?:\[\d+\])?;", fields)
        self.assertEqual(names, g.HDR_FIELDS)
        flags = re.findall(r"#define RF_(\w+)\s+BIT\((\d+)\)", src)
        self.assertEqual([n for n, _ in sorted(flags, key=lambda x: int(x[1]))], g.FLAGS)

    def test_generated_header_is_exact(self):
        text = self.hdr.read_text()
        self.assertIn("0xec, 0x60, 0xbc, 0x9f, 0xfd, 0xe8, 0x70, 0x42, 0x8c, 0x54", text)
        self.assertIn(f"J316S_GPULOG_FILE_OFFSET 0x{self.meta['file_offset']:x}ULL", text)
        self.assertEqual(self.meta["file_offset"] % BLOCK, 0)
        data = self.img.read_bytes()
        off = self.meta["file_offset"]
        self.assertEqual(data[off:off + 16], g.MAGIC)
        self.assertEqual(data[off + 16:off + 32].hex(), self.meta["token"])
        self.assertEqual(data[off + 32:off + g.FILE_BYTES], bytes(g.FILE_BYTES - 32))

    def test_replacing_boot_bin_keeps_log_in_place(self):
        (self.tmp / "boot.bin").write_bytes(b"y" * 900000)
        subprocess.run(["mcopy", "-o", "-i", str(self.img), str(self.tmp / "boot.bin"),
                        "::m1n1/boot.bin"], check=True)
        run("verify", self.img, "--meta", self.hdr.with_suffix(".json"))

    def test_fragmented_or_missing_file_is_rejected(self):
        subprocess.run(["mdel", "-i", str(self.img), "::m1n1/gpulog.bin"], check=True)
        with self.assertRaises(subprocess.CalledProcessError):
            run("verify", self.img, "--meta", self.hdr.with_suffix(".json"))

    def write_log(self, sequence=3, flags=(0, 1, 3, 11)):
        off = self.meta["file_offset"]
        adt = adt_node([("name", b"device-tree\0")], [
            adt_node([("name", b"arm-io\0")], [
                adt_node([("name", b"sgx\0"), ("compatible", b"gpu,t6000\0"),
                          ("interrupts", bytes.fromhex("1404000015040000"))]),
            ]),
        ])
        text = b"J316s GPU probe J316s-GPU-TEST\n"
        console = b"m1n1 console\n"
        rf = sum(1 << b for b in flags)
        hdr = struct.pack(g.HDR_FMT, g.MAGIC, bytes.fromhex(self.meta["token"]), 1, 256, 7, rf,
                          sequence, 123, 24000000, 0x1000, len(text), 0x80000, len(console),
                          0x100000, len(adt), len(adt), b"J316s-GPU-TEST", b"S7 complete")
        with open(self.img, "r+b") as f:
            f.seek(off)
            f.write(hdr)
            f.seek(off + 0x1000)
            f.write(text)
            f.seek(off + 0x80000)
            f.write(console)
            f.seek(off + 0x100000)
            f.write(adt)
        return text

    def test_decode_from_image_and_copy(self):
        text = self.write_log()
        out = self.tmp / "out"
        printed = run("decode", self.img, "--meta", self.hdr.with_suffix(".json"), "--out", out)
        summary = json.loads((out / "summary.json").read_text())
        self.assertEqual(summary["flags"], ["LOG_OPEN", "ADT_SAVED", "NODES_FOUND", "COMPLETE"])
        self.assertEqual((out / "probe.txt").read_bytes(), text)
        gpu = json.loads((out / "adt-gpu.json").read_text())
        self.assertEqual(gpu["/arm-io/sgx"]["compatible"], '"gpu,t6000"')
        self.assertIn('"stage": 7', json.dumps(summary))
        copy = self.tmp / "gpulog.bin"
        off = self.meta["file_offset"]
        copy.write_bytes(self.img.read_bytes()[off:off + g.FILE_BYTES])
        run("decode", copy, "--out", self.tmp / "out2")
        self.assertIn("decoded", printed)

    def test_unwritten_log_is_reported(self):
        printed = run("decode", self.img, "--meta", self.hdr.with_suffix(".json"), "--out",
                      self.tmp / "o")
        self.assertIn("never written", printed)


if __name__ == "__main__":
    unittest.main()
