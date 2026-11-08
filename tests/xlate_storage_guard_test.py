"""Regression tests for the packed-half atomic add and the halfword store (storage candidates).

Each test replays one captured shader-failure packet through the translator (tools/replay_shader_failure.py), then runs
spirv-val --target-env vulkan1.3 on the module it writes. The packets are game-derived shader code and are not in the
repository. If the translator, the packets or spirv-val are missing, the whole class is skipped and the skip message
names what is missing. With all of them present, both tests run.

Environment:
  XLATE_TRANSLATOR  translator under test (default: build/xlate/totk_xlate.exe)
  XLATE_PACKETS     folder that holds run-63399/ and run-d0762/ (default: build/packets)
  SPIRV_VAL         spirv-val executable (default: spirv-val on PATH)

Observed tuples (translator error text, family from the seed split):
  * RED.E.ADD.F16x2.FTZ.RN in family 63399bcafafc05041e92103533f23610f1605f3c65fa98a32f6900335e645bb1: a guarded
    32-bit compare-exchange of the packed-half add (GlobalToStorage.PackedHalfAddFtzRn). Must translate and validate.
  * STG.E.U16 in family d0762eff0fa4b2b4f2d55e566722867d757099ade6692d7e9391b5a674a6ddef: a guarded halfword store
    (byte-lane compare-exchange, two words when it crosses). Same shader also contains the RED above. Must translate.

Run:
  python -m unittest tests.xlate_storage_guard_test
"""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
PACKET_ROOT = pathlib.Path(os.environ.get("XLATE_PACKETS", ROOT / "build" / "packets"))
TRANSLATOR = pathlib.Path(os.environ.get("XLATE_TRANSLATOR", ROOT / "build" / "xlate" / "totk_xlate.exe"))
SPIRV_VAL = os.environ.get("SPIRV_VAL", "spirv-val")
REPLAY = ROOT / "tools" / "replay_shader_failure.py"

PACKETS = {
    "red-f16x2": PACKET_ROOT / "run-63399" / "failures" / "capture-32004-1"
                 / "2c0b4eb6a04d24243fb4c7e2c0809f4f4a54142a371c76fe42d01ae5f228d6f2.request.json",
    "store-u16": PACKET_ROOT / "run-d0762" / "failures" / "capture-17724-1"
                 / "9730318e3e18d6540ba4de0ffc64e50499acc26511d32b11f54305cc4f960b69.request.json",
}


def replay(packet, out_dir):
    """Replay one packet. Returns (exit code, stdout, stderr).

    The replay script exits 0 only when stdout is exactly 'ok <key>'. Its stderr can carry diagnostics (it sets
    DMSH_STACK=1 in the translator), so the verdict is taken from stdout and stderr is shown only on failure.
    """
    run = subprocess.run(
        [sys.executable, str(REPLAY), str(packet), "--translator", str(TRANSLATOR), "--out", str(out_dir)],
        capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=600)
    return run.returncode, run.stdout, run.stderr


class StorageGuardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # The packets are game-derived and not in the repository, so a checkout without them still passes: the class is
        # skipped with the reason. With the translator, the packets and spirv-val present, it runs in full.
        missing = [str(p) for p in (TRANSLATOR, *PACKETS.values()) if not p.is_file()]
        if missing:
            raise unittest.SkipTest("needs a translator and captured packets (set XLATE_TRANSLATOR and XLATE_PACKETS). "
                                    "Missing: " + ", ".join(missing))
        cls.spirv_val = shutil.which(SPIRV_VAL)
        if cls.spirv_val is None:
            raise unittest.SkipTest("spirv-val not found; put it on PATH or set SPIRV_VAL")

    def _translate_and_validate(self, name):
        with tempfile.TemporaryDirectory() as out:
            code, stdout, stderr = replay(PACKETS[name], out)
            self.assertEqual(code, 0, f"stdout:\n{stdout}\nstderr:\n{stderr}")
            self.assertTrue(stdout.startswith("ok "), stdout)
            modules = sorted(pathlib.Path(out).glob("*.spv"))
            self.assertEqual(len(modules), 1, "the translator must write exactly one SPIR-V module")
            val = subprocess.run([self.spirv_val, "--target-env", "vulkan1.3", str(modules[0])],
                                 capture_output=True, text=True)
            self.assertEqual(val.returncode, 0, val.stdout + val.stderr)

    def test_red_f16x2_packed_half_add_translates_and_validates(self):
        self._translate_and_validate("red-f16x2")

    def test_store_u16_halfword_store_translates_and_validates(self):
        self._translate_and_validate("store-u16")


if __name__ == "__main__":
    unittest.main()
