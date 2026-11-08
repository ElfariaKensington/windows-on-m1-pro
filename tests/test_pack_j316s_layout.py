import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import pack_j316s_setup as pack


class GuestRamDeviceOverlapTests(unittest.TestCase):
    def test_current_layout_keeps_ram_clear_of_devices(self):
        pack.verify_device_overlap(pack.setup_constants())

    def test_12_gib_ram_is_rejected_because_it_covers_xhci1(self):
        layout = dict(pack.setup_constants(), RAM_SIZE=0x300000000)
        with self.assertRaisesRegex(ValueError, "XHCI1"):
            pack.verify_device_overlap(layout)


if __name__ == "__main__":
    unittest.main()
