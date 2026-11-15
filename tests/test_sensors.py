"""Sensor behavior and CLI regressions; only Python's standard library is needed."""
from pathlib import Path
import subprocess
import tempfile
import unittest

BINARY = Path(__file__).resolve().parents[1] / 'wmpixeltemp'

class Sensors(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
    def tearDown(self):
        self.tmp.cleanup()
    def put(self, path, value):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(str(value) + '\n')
        return str(target)
    def run_app(self, *args):
        return subprocess.run([str(BINARY), '--sysfs-root', str(self.root), *args],
                              text=True, capture_output=True, check=False)
    def test_missing_is_not_zero(self):
        self.assertEqual(self.run_app('--once').stdout, 'CPU=N/A GPU=N/A unit=C\n')
    def test_hottest_cpu_excludes_unrelated_device(self):
        self.put('class/hwmon/hwmon0/name', 'coretemp')
        self.put('class/hwmon/hwmon0/temp1_input', 47500)
        self.put('class/hwmon/hwmon0/temp2_input', 52000)
        self.put('class/hwmon/hwmon1/name', 'nvme')
        self.put('class/hwmon/hwmon1/temp1_input', 99000)
        self.assertEqual(self.run_app('--once').stdout, 'CPU=52 GPU=N/A unit=C\n')
    def test_thermal_fallback(self):
        self.put('class/thermal/thermal_zone0/type', 'acpitz')
        self.put('class/thermal/thermal_zone0/temp', 99000)
        self.put('class/thermal/thermal_zone1/type', 'x86_pkg_temp')
        self.put('class/thermal/thermal_zone1/temp', 43000)
        self.assertIn('CPU=43 ', self.run_app('--once').stdout)
    def test_gpu_index_and_drm_fallback(self):
        self.put('class/drm/card2/device/hwmon/hwmon9/temp1_input', 61500)
        self.assertIn('GPU=62 ', self.run_app('--gpu-index', '2', '--once').stdout)
        self.assertIn('GPU=N/A ', self.run_app('--gpu-index', '1', '--once').stdout)
    def test_explicit_sensor_and_zero(self):
        cpu = self.put('cpu', 0)
        gpu = self.put('gpu', 100000)
        self.assertEqual(self.run_app('--cpu-file', cpu, '--gpu-file', gpu, '--once').stdout,
                         'CPU=0 GPU=100 unit=C\n')
    def test_bad_readings(self):
        for value in ['N/A', '', '-1000', '151000', '49000garbage', '99999999999999999999999']:
            path = self.put('bad', value)
            self.assertIn('CPU=N/A ', self.run_app('--cpu-file', path, '--once').stdout)
    def test_bad_arguments(self):
        for args in [('--scale', '0'), ('--scale', '9'), ('--interval', 'nan'),
                     ('--gpu-index', '-1'), ('--cpu-file',), ('--unknown',)]:
            self.assertEqual(self.run_app(*args).returncode, 2, args)
    def test_snapshot_and_demo_label(self):
        out = self.root / 'demo.ppm'
        result = self.run_app('--demo', '--once', '--scale', '3', '--snapshot', str(out))
        self.assertEqual(result.returncode, 0)
        self.assertIn('CPU=48 GPU=62 unit=C DEMO', result.stdout)
        header, data = out.read_bytes().split(b'255\n', 1)
        self.assertEqual(header, b'P6\n192 192\n')
        self.assertEqual(len(data), 192 * 192 * 3)

if __name__ == '__main__':
    unittest.main()
