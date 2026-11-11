"""Protect evidence from the one-shot J316s driver installation."""
import os
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
GPU = ROOT / 'drivers/apple-agx/render-admission'


class StartupDiagnosticsTests(unittest.TestCase):
    def test_install_keeps_first_start_and_finalizes_trace(self):
        script = (GPU / 'j316s/gpu.ps1').read_text()
        install = script.split("if ($Action -eq 'install') {", 1)[1].split("elseif", 1)[0]
        self.assertNotIn('Restore-Display', script)
        self.assertNotIn('keybd_event', script)
        self.assertLess(install.index('Start-StartupTrace'), install.index('pnputil /delete-driver'))
        self.assertLess(install.index('Start-StartupTrace'), install.index('-Name G3Armed'))
        self.assertIn('finally { Stop-StartupTrace }', install)
        self.assertIn('$dx.WaitForExit(30000)', script)

    def test_query_result_survives_generic_lifecycle_receipts(self):
        receipts = (GPU / 'src/receipts.c').read_text()
        query = receipts.split('void AdmissionRecordQuery(', 1)[1].split('\n#if defined(APPLE_AGX_GPUVA_G3_QUALIFICATION)', 1)[0]
        self.assertIn('WriteDword(key, L"Wom1LastQueryStatus", (ULONG)Status)', query)
        self.assertEqual(receipts.count('L"Wom1LastQueryStatus"'), 1)

    def test_powershell_parsing_and_profile_regression(self):
        pwsh = os.environ.get('PWSH') or shutil.which('pwsh')
        local = ROOT / '.build/tools/pwsh/pwsh'
        if not pwsh and local.is_file():
            pwsh = str(local)
        if not pwsh:
            self.skipTest('PowerShell is required for script execution')
        for script in (GPU / 'j316s/gpu.ps1', ROOT / 'drivers/j316-wificx/wifi-connect.ps1'):
            command = "$e=$null; [void][Management.Automation.Language.Parser]::ParseFile($args[0], [ref]$null, [ref]$e); if ($e.Count) { $e; exit 1 }"
            # -Command positional arguments are PowerShell expressions, so use
            # an environment variable for the path instead.
            command = command.replace('$args[0]', '$env:J316_SCRIPT')
            subprocess.run([pwsh, '-NoProfile', '-Command', command],
                           env={**os.environ, 'J316_SCRIPT': str(script)}, check=True,
                           capture_output=True, text=True, timeout=30)
        subprocess.run([pwsh, '-NoProfile', '-File', str(ROOT / 'tests/test_j316s_wlan_profile.ps1')],
                       check=True, capture_output=True, text=True, timeout=30)


if __name__ == '__main__':
    unittest.main()
