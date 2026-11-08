"""Tests for the perf harness: the gate-14 settings-file check and the culture-safe number formatting of perf_run.ps1."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import perf_evidence  # noqa: E402


class IniDiagnosticsTests(unittest.TestCase):
    def test_off_by_default(self):
        self.assertEqual(perf_evidence.ini_diagnostics('[display]\nfullscreen = 0\nwindow_size = 1920x1080\n'), [])

    def test_switches_that_are_on(self):
        text = '[diagnostics]\nflight = 1\nTOTK_TRACE = on\nprofile = "1"\nvk_validation = 1\n'
        self.assertEqual(sorted(perf_evidence.ini_diagnostics(text)), ['FLIGHT', 'PROFILE', 'TRACE', 'VK_VALIDATION'])

    def test_later_line_wins_like_cfg_c(self):
        # runtime/cfg.c add(): a repeated key takes the later line, so the switch is off here and on in the second case.
        self.assertEqual(perf_evidence.ini_diagnostics('flight = 1\nflight = 0\n'), [])
        self.assertEqual(perf_evidence.ini_diagnostics('flight = 0\nflight = 1\n'), ['FLIGHT'])

    def test_empty_later_line_leaves_the_earlier_value(self):
        # cfg.c: "key =" leaves the setting at its default, it does not reset an earlier line.
        self.assertEqual(perf_evidence.ini_diagnostics('flight = 1\nflight =\n'), ['FLIGHT'])

    def test_dkb_prefix_is_not_a_file_key(self):
        # cfg.c norm_key strips only TOTK_, so DKB_FLIGHT in the file is an unknown key and switches nothing on.
        self.assertEqual(perf_evidence.ini_diagnostics('DKB_FLIGHT = 1\n'), [])

    def test_off_values_and_empty_do_not_count(self):
        text = 'flight = 0\ntrace = off\nprofile =\nvk_validation = false\ncpu_branch_evidence = no\n'
        self.assertEqual(perf_evidence.ini_diagnostics(text), [])

    def test_comments_and_sections_are_ignored(self):
        text = '# flight = 1\n; trace = 1\n[profile]\n'
        self.assertEqual(perf_evidence.ini_diagnostics(text), [])

    def test_other_keys_are_not_diagnostic_switches(self):
        self.assertEqual(perf_evidence.ini_diagnostics('flight_frames = 1200\nflight_dir = x\nfps_limit = 60\n'), [])

    def test_none_and_empty_text(self):
        self.assertEqual(perf_evidence.ini_diagnostics(None), [])
        self.assertEqual(perf_evidence.ini_diagnostics(''), [])


class EffectiveSettingsTests(unittest.TestCase):
    def test_dkb_name_maps_to_totk_name(self):
        self.assertEqual(perf_evidence.effective_settings({'DKB_PROFILE': '1'}).get('TOTK_PROFILE'), '1')

    def test_dkb_wins_over_totk(self):
        got = perf_evidence.effective_settings({'TOTK_PROFILE': '0', 'DKB_PROFILE': '1'})
        self.assertEqual(got['TOTK_PROFILE'], '1')

    def test_other_names_are_kept(self):
        got = perf_evidence.effective_settings({'TOTK_FPS_LIMIT': '60'})
        self.assertEqual(got, {'TOTK_FPS_LIMIT': '60'})


@unittest.skipUnless(shutil.which('pwsh'), 'PowerShell 7 not installed')
class PerfRunScriptTests(unittest.TestCase):
    def test_script_parses(self):
        cmd = ["pwsh", "-NoProfile", "-Command",
               "$e = $null; [void][System.Management.Automation.Language.Parser]::ParseFile("
               f"'{ROOT / 'tools' / 'perf_run.ps1'}', [ref]$null, [ref]$e); exit $e.Count"]
        self.assertEqual(subprocess.run(cmd, capture_output=True, text=True).returncode, 0)

    def test_numbers_use_the_invariant_culture(self):
        text = (ROOT / 'tools' / 'perf_run.ps1').read_text(encoding='utf-8-sig')
        self.assertIn('[Globalization.CultureInfo]::InvariantCulture', text)
        # The samples CSV rows must not be built with the culture-dependent -f operator and N1/F1.
        self.assertNotIn("{0:N1}", text)
        self.assertNotIn("' -f ($p.WorkingSet64", text)

    def test_invariant_formatting_under_pt_br(self):
        cmd = ["pwsh", "-NoProfile", "-Command",
               "[Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::GetCultureInfo('pt-BR');"
               "[string]::Format([Globalization.CultureInfo]::InvariantCulture, '{0:F1},{1:F1}', @(1911.04, 1234.5))"]
        out = subprocess.run(cmd, capture_output=True, text=True).stdout.strip()
        self.assertEqual(out, '1911.0,1234.5')


if __name__ == '__main__':
    unittest.main()
