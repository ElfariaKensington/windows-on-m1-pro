import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/install-esp.sh"


class InstallEspTests(unittest.TestCase):
    def run_dry(self, *arguments):
        return subprocess.run(
            [str(SCRIPT), "--dry-run", *arguments],
            cwd="/tmp",
            text=True,
            capture_output=True,
            check=False,
        )

    def test_install_is_scoped_backed_up_atomic_and_verified(self):
        result = self.run_dry(
            "install", "--disk", "disk9s4", "--image", "dist/j313/boot.bin"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        output = result.stdout
        for item in (
            "validate outer bootstrap manifest",
            "validate nested standalone manifest",
            "diskutil mount disk9s4",
            "/m1n1/boot.bin",
            "create backup once",
            "copy to temporary sibling",
            "verify SHA-256",
            "atomic rename",
        ):
            self.assertIn(item, output)
        self.assertNotIn("disk0s4", output)
        self.assertNotIn("/Users/pavel", output)

    def test_restore_uses_the_same_scoped_backup(self):
        result = self.run_dry("restore", "--disk", "disk9s4")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("restore backup", result.stdout)
        self.assertIn("verify SHA-256", result.stdout)

    def test_install_rejects_m1_pro_before_contacting_diskutil(self):
        with tempfile.TemporaryDirectory() as directory:
            for name, body in (
                ("sysctl", "echo MacBookPro18,1"),
                ("diskutil", "echo 'diskutil was called'; exit 99"),
            ):
                command = Path(directory) / name
                command.write_text(f"#!/bin/sh\n{body}\n")
                command.chmod(0o755)
            result = subprocess.run(
                [str(SCRIPT), "install", "--disk", "disk9s4", "--image", "unused.bin"],
                cwd=directory,
                env={**os.environ, "PATH": f"{directory}{os.pathsep}{os.defpath}"},
                text=True,
                capture_output=True,
            )
        self.assertEqual(result.returncode, 1)
        self.assertIn("requires MacBookAir10,1 (detected MacBookPro18,1)", result.stderr)
        self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    unittest.main()
