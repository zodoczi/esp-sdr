"""Bounded search, register preservation and failed-lock recovery checks."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('cc'), 'Host C compiler unavailable')
class PLLCalibration(unittest.TestCase):
    def test_capacitor_search(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / 'pll-calibration'
            subprocess.run([
                'cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=undefined', '-DCONFIG_IDF_TARGET_ESP32C61=1',
                '-I' + str(ROOT / 'main/common'),
                str(ROOT / 'tests/pll_calibration_test.c'), '-o', str(exe),
            ], check=True)
            subprocess.run([str(exe)], check=True)
