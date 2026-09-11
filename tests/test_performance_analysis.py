"""Synthetic timing records; no copyrighted game content or hardware claims."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('performance', Path(__file__).resolve().parents[1] / 'scripts/analyze-performance.py')
performance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(performance)


def sample(wall, guest, calls=0, ms=0, phase='heartbeat', scale=1):
    return (f'INFO: Boot progress: {phase} wall_ms={wall} core_state=2 guest_ms={guest}\n'
            f'INFO: Performance scope: {phase} metric=vk_present interval_calls=0 interval_ms=0 boot_calls={calls} boot_ms={ms} '
            f'boot_cpu_calls={calls} boot_cpu_ms={ms / 2} sample_scale={scale}\n')


class PerformanceAnalysisTests(unittest.TestCase):
    def test_same_elapsed_window_not_unweighted_mean(self):
        log = 'INFO: Boot stage 1: synthetic\nINFO: Vulkan boot: submission worker=1\n'
        log += sample(1, 19999) + sample(1000, 20000, 1, 1)
        log += sample(6000, 24000, 6, 2001) + sample(16000, 28000, 16, 7001)
        log += sample(99000, 60001)
        result = performance.analyze(log)[0]
        self.assertAlmostEqual(result['window']['speed_pct'], 100 * 8000 / 15000)
        self.assertNotEqual(result['window']['speed_pct'], (80 + 40) / 2)
        self.assertEqual(result['sampled_range_pct'], [40, 80])
        self.assertEqual(result['window']['scopes']['vk_present']['ms'], 7000)
        self.assertEqual(result['window']['scopes']['vk_present']['cpu_ms'], 3500)
        self.assertEqual(result['slowest_quartile'][0]['speed_pct'], 40)
        self.assertTrue(result['worker'])

    def test_boots_do_not_mix_and_incomplete_window_is_explicit(self):
        log = 'INFO: Boot stage 1: one\n' + sample(1, 21000)
        log += 'INFO: Boot stage 1: two\nINFO: Vulkan boot: submission worker=0\n'
        log += sample(10, 21000) + sample(1010, 22000)
        log += sample(2010, 23000, phase='after-join')
        results = performance.analyze(log)
        self.assertIsNone(results[0]['window'])
        self.assertFalse(results[1]['worker'])
        self.assertTrue(results[1]['clean_join'])
        self.assertEqual(results[1]['window']['speed_pct'], 100)

    def test_raw_sampling_and_unavailable_cpu_not_faked(self):
        log = 'INFO: Boot stage 1: sampled\n' + sample(10, 21000, scale=64)
        log += sample(1010, 22000, 2, 10, scale=64)
        result = performance.analyze(log)[0]['window']['scopes']['vk_present']
        self.assertEqual(result['ms'], 10)
        self.assertEqual(result['wall_fraction_pct_estimate'], 64)
        legacy = log.replace('boot_cpu_calls=2 boot_cpu_ms=5.0 ', '')
        result = performance.analyze(legacy)[0]['window']['scopes']['vk_present']
        self.assertIsNone(result['cpu_fraction_pct_estimate'])

    def test_non_monotonic_progress_does_not_report_gain(self):
        log = 'INFO: Boot stage 1: reset\n' + sample(10, 22000) + sample(20, 21000)
        self.assertIsNone(performance.analyze(log)[0]['window'])


if __name__ == '__main__':
    unittest.main()
