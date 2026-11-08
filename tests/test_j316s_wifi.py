"""Regression checks for the J316s Wi-Fi bridge ABI and its two Windows drivers."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BRIDGE = ROOT / "drivers/j316-wifi/bridge.h"
LEGACY = ROOT / "drivers/j316-wifi"
NATIVE = ROOT / "drivers/j316-wificx"
PACKAGES = ROOT / ".build/j316s-fresh-input"

ABI_CHECK = r"""
#include <stdint.h>
#include "bridge.h"
#define LINE(x) ((x) / 128u)
/* Host-written native registers share no 128-byte line with guest-written ones: the host
 * cleans and invalidates whole lines after its stores (hv_j316s_wifi.c store_reg). */
_Static_assert(LINE(JW_REG_MODE) == 1 && LINE(JW_REG_CTL_SEQ) == 1 && LINE(JW_REG_CTL_CMD) == 1 &&
               LINE(JW_REG_CTL_LEN) == 1 && LINE(JW_REG_EV_CONS) == 1 &&
               LINE(JW_REG_N_TX_PROD) == 1 && LINE(JW_REG_N_RX_CONS) == 1, "guest line");
_Static_assert(LINE(JW_REG_MODE_ACK) == 0 && LINE(JW_REG_CTL_DONE) == 0 &&
               LINE(JW_REG_CTL_STATUS) == 0 && LINE(JW_REG_CTL_RESULT_LEN) == 0 &&
               LINE(JW_REG_EV_PROD) == 0 && LINE(JW_REG_EV_DROPS) == 0, "host line");
_Static_assert(JW_REG_EV_DROPS < JW_REG_MODE && JW_REG_N_RX_CONS < JW_CONFIG_SSID_LEN, "registers");
_Static_assert(JW_REG_CTL_SEQ > JW_REG_COMMAND_STATUS && JW_REG_MODE_ACK > JW_REG_COMMAND_STATUS, "v1 regs");
_Static_assert(JW_CONFIG_PSK + 64 <= JW_CTL_BUF && JW_CTL_BUF + JW_CTL_MAX <= JW_TX_BASE, "ctl");
_Static_assert(JW_RX_BASE + JW_RING_COUNT * JW_SLOT_SIZE <= JW_EV_BASE, "rx/event");
_Static_assert(JW_EV_BASE + JW_EV_COUNT * JW_EV_SLOT <= JW_SIZE, "event ring in window");
_Static_assert(JW_CTL_MAX >= 8192u, "BCM_CTL_SIZE command fits");
int main(void) { return 0; }
"""


class BridgeAbiTests(unittest.TestCase):
    def test_native_register_and_region_layout(self):
        compiler = shutil.which(os.environ.get("CC", "clang"))
        if not compiler:
            self.skipTest("A C compiler is required")
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "abi.c"
            source.write_text(ABI_CHECK)
            subprocess.run([compiler, "-std=c11", "-Werror", "-I", str(BRIDGE.parent),
                            str(source), "-o", str(Path(tmp) / "abi")], check=True,
                           capture_output=True)


class LegacyBridgeDpcTests(unittest.TestCase):
    """0x133 DPC_WATCHDOG_VIOLATION (minidump 040124-25484-01): the poll DPC copied
    frames one uncached byte at a time and could outlast its 5 ms period."""

    def test_poll_uses_wide_copies_and_a_time_budget(self):
        text = (LEGACY / "driver.c").read_text()
        poll = text[text.index("static VOID JwPoll("):text.index("static VOID JwSend(")]
        self.assertNotIn("READ_REGISTER_BUFFER_UCHAR", poll)
        self.assertNotIn("WRITE_REGISTER_BUFFER_UCHAR(a->Registers + slot", poll)
        self.assertIn("JwSlotRead(a, slot, length)", poll)
        self.assertEqual(poll.count("JwOverBudget(a, start)"), 2)
        budget = int(re.search(r"#define JW_POLL_BUDGET_100NS (\d+)u", text).group(1))
        self.assertLessEqual(budget, 20000)   # <= 2 ms of a 5 ms period


def _wdk_available():
    return (PACKAGES / "microsoft.windows.wdk.arm64").is_dir() and \
        Path(os.environ.get("CLANG_CL", "/opt/homebrew/opt/llvm/bin/clang-cl")).exists()


@unittest.skipUnless(_wdk_available(), "pinned WDK and clang-cl are required")
class DriverBuildTests(unittest.TestCase):
    def _imports(self, image):
        readobj = shutil.which("llvm-readobj") or "/opt/homebrew/opt/llvm/bin/llvm-readobj"
        out = subprocess.run([readobj, "--coff-imports", str(image)], check=True,
                             capture_output=True, text=True).stdout
        return set(re.findall(r"Name: (\S+)", out))

    def test_native_wificx_driver_builds_with_inbox_bindings_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run(["bash", str(NATIVE / "build.sh")], check=True, capture_output=True,
                           env={**os.environ, "JX_OUTPUT": tmp})
            self.assertEqual(self._imports(Path(tmp) / "J316WifiCx.sys"),
                             {"ntoskrnl.exe", "HAL.dll", "WDFLDR.SYS"})

    def test_legacy_bridge_driver_builds(self):
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run(["bash", str(LEGACY / "build.sh")], check=True, capture_output=True,
                           env={**os.environ, "JW_OUTPUT": tmp})
            self.assertTrue((Path(tmp) / "J316Wifi.sys").stat().st_size > 0)


class NativeInfTests(unittest.TestCase):
    def test_inf_binds_same_device_as_native_wlan(self):
        inf = (NATIVE / "J316WifiCx.inf").read_text()
        self.assertIn("ACPI\\APPL0010", inf)
        for directive in ("*IfType             = 71", "*MediaType          = 16",
                          "*PhysicalMediaType  = 9", "KmdfLibraryVersion = 1.33"):
            self.assertIn(directive, inf)
        legacy = (LEGACY / "J316Wifi.inf").read_text()
        new_ver = re.search(r"DriverVer\s*=\s*([\d/]+),([\d.]+)", inf).groups()
        old_ver = re.search(r"DriverVer\s*=\s*([\d/]+),([\d.]+)", legacy).groups()
        self.assertGreater(tuple(map(int, new_ver[1].split("."))), tuple(map(int, old_ver[1].split("."))))


class CatalogTests(unittest.TestCase):
    """The macOS catalog generator must stay byte-identical to makecat (installed J316 input
    package, build-v5) apart from the random list identifier and timestamp."""

    REF = PACKAGES / "build-v5"

    def test_matches_makecat_reference(self):
        if not (self.REF / "J316SpiInput-unsigned.cat").exists():
            self.skipTest("makecat reference catalog not present")
        import datetime
        import importlib.util
        spec = importlib.util.spec_from_file_location("catalog", ROOT / "tools/make_driver_catalog.py")
        catalog = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(catalog)
        ref = (self.REF / "J316SpiInput-unsigned.cat").read_bytes()
        out = catalog.build([self.REF / "J316SpiInput.sys", self.REF / "J316SpiInput.inf",
                             self.REF / "J316VhfInput.inf"], ref[67:83],
                            datetime.datetime.strptime("261107200627", "%y%m%d%H%M%S"))
        self.assertEqual(out, ref)


class NativeDriverContractTests(unittest.TestCase):
    """Regressions found from guest evidence (2026-11-08, J316WifiCx.log, minidumps, WLAN log)."""

    def setUp(self):
        self.src = (NATIVE / "driver.cpp").read_text()

    def test_station_peer_demux_range_is_one(self):
        # Minidumps 110826-23171/25656/26234: 0x7E c0000420 at WifiCx.sys+0xbe5bc, an assert in
        # WifiAdapterInitialize that a station adapter's peer-address demux has Range 1.
        self.assertRegex(self.src, r"#define JX_PEER_DEMUX_RANGE 1\b")
        self.assertIn("WIFI_ADAPTER_TX_PEER_ADDRESS_DEMUX_INIT(&demux, JX_PEER_DEMUX_RANGE)", self.src)

    def test_media_state_follows_association(self):
        # WiFiCx never sets the NetAdapterCx link state; without it EAPOL 2/4 is never sent
        # (log: rx 1/4 retransmits, tx 0; WLAN-AutoConfig 11006 0x00048005).
        self.assertIn("NetAdapterSetLinkState(d->Adapter, &s)", self.src)
        finish = self.src[self.src.index("static void FinishConnect("):self.src.index("static void Disassociate(")]
        self.assertLess(finish.index("IndicateAssociation(d, WDI_ASSOC_STATUS_SUCCESS"), finish.index("SetLink(d, true)"))
        self.assertLess(finish.index("SetLink(d, true)"), finish.index("CompleteTask(d, WDI_INDICATION_CONNECT_COMPLETE"))
        peer = self.src[self.src.index("static void PeerSync("):self.src.index("static void IndicateDisassociation(")]
        self.assertIn("SetLink(d, false)", peer)

    def test_mixed_wpa_wpa2_group_tkip(self):
        # TurkTelekom AP (RSN group TKIP): SET_SSID status 1 with a CCMP-only group.
        self.assertIn("{DOT11_AUTH_ALGO_RSNA_PSK, DOT11_CIPHER_ALGO_TKIP}", self.src)
        self.assertIn("BCM_WSEC_TKIP | BCM_WSEC_AES", self.src)
        self.assertIn("if (d->GroupTkip) d->RsnIe[7] = 2;", self.src)
        self.assertIn("Put32(key + 112, BCM_CRYPTO_TKIP)", self.src)

    def test_release_does_not_abort_its_own_disassociate(self):
        release = self.src[self.src.index("static NTSTATUS EvtReleaseHardware("):self.src.index("static EVT_WDF_DRIVER_DEVICE_ADD")]
        self.assertLess(release.index("InterlockedExchange(&d->Stop, 0)"), release.index("BCM_C_DISASSOC"))

    def test_bridge_inf_is_catalog_signed(self):
        self.assertIn("CatalogFile = J316Wifi.cat", (LEGACY / "J316Wifi.inf").read_text())


if __name__ == "__main__":
    unittest.main()
