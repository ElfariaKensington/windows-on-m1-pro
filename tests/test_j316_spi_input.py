"""Behavioral regression checks for the independent J316 SPI input driver."""
import os
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import plistlib

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "drivers/j316-spi-input"


class J316SpiProtocolTests(unittest.TestCase):
    def test_protocol_and_touch_reports(self):
        compiler = shutil.which(os.environ.get("CC", "clang"))
        if not compiler:
            self.skipTest("A C compiler is required")
        with tempfile.TemporaryDirectory() as tmp:
            executable = Path(tmp) / "protocol-test"
            subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-g",
                            str(SOURCE / "protocol.c"), str(SOURCE / "protocol_test.c"),
                            "-o", str(executable)], check=True, capture_output=True)
            result = subprocess.run([str(executable)], check=True, capture_output=True, text=True)
            self.assertIn("100000 malformed packets PASS", result.stdout)


class J316DiscoveryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which(os.environ.get("CC", "clang"))
        if not compiler:
            raise unittest.SkipTest("A C compiler is required")
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.executable = Path(cls.temp.name) / "discovery-test"
        subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-g", str(SOURCE / "protocol.c"),
                        str(SOURCE / "discovery_test.c"), "-o", str(cls.executable)],
                       check=True, capture_output=True)

    def test_per_type_reply_counter_and_interleaved_input(self):
        subprocess.run([str(self.executable), "replies"], check=True, capture_output=True)

    def test_optional_metadata_and_required_identity_descriptors(self):
        subprocess.run([str(self.executable), "metadata"], check=True, capture_output=True)


class J316StagingTests(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location("j316_stage", SOURCE / "stage.py")
        self.stage = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.stage)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.old, self.new = b"O" * 64, b"N" * 64
        self.device = self.root / "device"
        self.device.write_bytes(self.old)
        self.backup, self.image = self.root / "before.img", self.root / "after.img"
        self.backup.write_bytes(self.old)
        self.image.write_bytes(self.new)
        self.stage.DEVICE, self.stage.SIZE = self.device, 64
        self.manifest = self.root / "candidate.manifest.json"
        self.manifest.write_text(json.dumps({
            "rollback": {"path": str(self.backup), "sha256": hashlib.sha256(self.old).hexdigest()},
            "esp_image": {"path": str(self.image), "sha256": hashlib.sha256(self.new).hexdigest()},
        }))
        self.info = {"DiskUUID": self.stage.UUID, "TotalSize": 64, "Internal": True,
                     "Content": "EFI", "Mounted": False}
        self.writes = []

    def invoke(self, restore=False, corrupt=False):
        def command(args, **kwargs):
            return "MacBookPro18,1\n" if "sysctl" in args[0] else plistlib.dumps(self.info)

        def write(path):
            self.writes.append(path)
            self.device.write_bytes(b"!" * 64 if corrupt and path == self.image else path.read_bytes())

        with patch("sys.argv", ["stage.py", str(self.manifest)] + (["--restore"] if restore else [])), \
             patch.object(self.stage.os, "geteuid", return_value=0), \
             patch.object(self.stage.subprocess, "check_output", side_effect=command), \
             patch.object(self.stage, "resolve_device", return_value=self.device), \
             patch.object(self.stage, "write_image", side_effect=write), \
             contextlib.redirect_stdout(io.StringIO()):
            self.stage.main()

    def test_install_readback_and_restore(self):
        self.invoke()
        self.assertEqual(self.device.read_bytes(), self.new)
        receipt = json.loads(self.manifest.with_suffix(".installed.json").read_text())
        self.assertEqual(receipt["readback_sha256"], hashlib.sha256(self.new).hexdigest())
        self.invoke(restore=True)
        self.assertEqual(self.device.read_bytes(), self.old)

    def test_renumbered_internal_efi_device(self):
        self.assertEqual(self.stage.resolve_device({"DeviceIdentifier": "disk0s9"}),
                         Path("/dev/rdisk0s9"))

    def test_whole_or_external_disk_identifier_is_rejected(self):
        for identifier in ("disk0", "disk1s4", "disk0s0", "../disk0s4", ""):
            with self.subTest(identifier=identifier), self.assertRaises(RuntimeError):
                self.stage.resolve_device({"DeviceIdentifier": identifier})

    def test_changed_target_refuses_without_writing(self):
        self.info["DiskUUID"] = "another-disk"
        with self.assertRaisesRegex(RuntimeError, "identity changed"):
            self.invoke()
        self.assertEqual(self.writes, [])
        self.assertEqual(self.device.read_bytes(), self.old)

    def test_mounted_target_refuses_without_writing(self):
        self.info["Mounted"] = True
        with self.assertRaisesRegex(RuntimeError, "unmounted"):
            self.invoke()
        self.assertEqual(self.writes, [])

    def test_changed_installed_bytes_refuse_without_writing(self):
        self.device.write_bytes(b"?" * 64)
        with self.assertRaisesRegex(RuntimeError, "partition changed"):
            self.invoke()
        self.assertEqual(self.writes, [])

    def test_bad_image_refuses_without_writing(self):
        self.image.write_bytes(b"?" * 64)
        with self.assertRaisesRegex(RuntimeError, "verification failed"):
            self.invoke()
        self.assertEqual(self.writes, [])

    def test_failed_readback_restores_original_bytes(self):
        with self.assertRaisesRegex(RuntimeError, "readback mismatch"):
            self.invoke(corrupt=True)
        self.assertEqual(self.writes, [self.image, self.backup])
        self.assertEqual(self.device.read_bytes(), self.old)
