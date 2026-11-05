from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
import os
from pathlib import Path
import runpy
import sys
from types import ModuleType, SimpleNamespace
import unittest
from unittest.mock import Mock, patch


ROOT = Path(__file__).resolve().parents[1]


class LaunchContractProxyTests(unittest.TestCase):
    def test_proxy_uses_explicit_non_overlapping_opcodes(self):
        source = (ROOT / "m1n1_windows/proxyclient/m1n1/proxy.py").read_text()
        self.assertIn("P_HV_LAUNCH_PUBLISH = 0xc1d", source)
        self.assertIn("P_HV_LAUNCH_CAPTURE = 0xc1e", source)

    def test_c_dispatch_validates_descriptor_and_snapshot_sizes(self):
        source = (ROOT / "m1n1_windows/src/proxy.c").read_text()
        self.assertIn("sizeof(struct hv_launch_j313_descriptor)", source)
        self.assertIn("sizeof(struct hv_contract_snapshot)", source)
        self.assertIn("hv_launch_j313_publish_descriptor", source)
        self.assertIn("hv_launch_j313_capture", source)

    def test_user_interrupt_dumps_before_proxy_without_nested_request(self):
        hv_source = (ROOT / "m1n1_windows/src/hv.c").read_text()
        runner = (ROOT / "run_uefi.py").read_text()
        self.assertIn('printf("HV: User interrupt', hv_source)
        user_interrupt = hv_source.split('printf("HV: User interrupt', 1)[1].split(
            "hv_exc_proxy", 1
        )[0]
        self.assertIn("hv_watchdog_snapshot_dump();", user_interrupt)
        self.assertNotIn("p.hv_watchdog_dump()", runner)
        self.assertIn("return EXC_RET.UNHANDLED", runner)

    def test_legacy_launch_contract_fallback_is_explicit_and_fail_closed(self):
        runner = (ROOT / "run_uefi.py").read_text()

        self.assertIn("WOM1_ALLOW_LEGACY_LAUNCH_CONTRACT", runner)
        self.assertIn("except ProxyCommandError", runner)
        self.assertIn("legacy launch-contract opcodes unavailable", runner)
        self.assertIn("launch_contract_available = False", runner)
        self.assertIn("if not launch_contract_available:", runner)

    def test_assisted_launcher_rejects_j316s_before_cpufreq(self):
        modules = {
            name: ModuleType(name)
            for name in ("m1n1.proxy", "m1n1.proxyutils", "m1n1.utils", "m1n1.hv")
        }
        proxy = Mock()
        modules["m1n1.proxy"].UartInterface = object
        modules["m1n1.proxy"].M1N1Proxy = Mock(return_value=proxy)
        modules["m1n1.proxyutils"].bootstrap_port = Mock()
        modules["m1n1.proxyutils"].ProxyUtils = Mock(return_value=SimpleNamespace(
            adt=SimpleNamespace(compatible=["J316sAP", "MacBookPro18,1"]),
        ))
        modules["m1n1.hv"].HV = Mock()
        modules["m1n1.hv"].TraceMode = object
        runner = ROOT / "run_uefi.py"
        args = [str(runner), "unused.fd", "--device", "mock-device"]
        error = StringIO()

        with (
            patch.dict(sys.modules, modules),
            patch.dict(os.environ),
            patch.object(sys, "argv", args),
            patch.object(sys, "path", list(sys.path)),
            redirect_stdout(StringIO()),
            redirect_stderr(error),
            self.assertRaises(SystemExit) as rejected,
        ):
            runpy.run_path(str(runner), run_name="__main__")

        self.assertEqual(rejected.exception.code, 2)
        self.assertIn("requires a J313 Mac", error.getvalue())
        proxy.cpufreq_init.assert_not_called()


if __name__ == "__main__":
    unittest.main()
