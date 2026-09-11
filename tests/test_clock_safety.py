import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
POLICY = ROOT / "upstream/dolphin/Source/Core/Horizon/ClockPolicy.h"
POLICY_IMPL = ROOT / "upstream/dolphin/Source/Core/Horizon/ClockPolicy.cpp"
CONTROL = ROOT / "upstream/dolphin/Source/Core/Horizon/ClockControl.cpp"


class ClockSafetyTests(unittest.TestCase):
    def test_writable_domain_has_no_memory_member(self):
        text = POLICY.read_text(encoding="utf-8")
        domain = re.search(r"enum class Domain.*?\{(.*?)\};", text, re.S)
        self.assertIsNotNone(domain)
        self.assertIn("CPU", domain.group(1))
        self.assertIn("GPU", domain.group(1))
        self.assertNotIn("MEM", domain.group(1))

    def test_platform_has_no_memory_set_path(self):
        text = CONTROL.read_text(encoding="utf-8")
        self.assertNotRegex(text, r"pcvSetClockRate\s*\(\s*PcvModule_EMC")
        self.assertNotRegex(text, r"clkrstSetClockRate\s*\(\s*&m_memory")
        self.assertNotIn("SetMemory", text)
        self.assertNotIn("RequestMemory", text)
        self.assertIn("pcvGetClockRate(PcvModule_EMC", text)
        self.assertIn("MEM writes=0", text)

    def test_targets_and_override_are_isolated(self):
        text = POLICY.read_text(encoding="utf-8")
        implementation = POLICY_IMPL.read_text(encoding="utf-8")
        self.assertIn("PERFORMANCE_CPU_TARGET_HZ = 1'785'000'000", text)
        self.assertIn("PERFORMANCE_GPU_TARGET_HZ = 921'600'000", text)
        self.assertIn("LEGACY_CPU_TARGET_HZ = 1'581'000'000", text)
        self.assertIn("LEGACY_GPU_TARGET_HZ = 614'400'000", text)
        self.assertIn("phocoena_auto_clocks=0", implementation)


if __name__ == "__main__":
    unittest.main()
