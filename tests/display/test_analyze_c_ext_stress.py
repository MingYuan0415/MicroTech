#!/usr/bin/env python3
"""Tests for the C_EXT_STRESS hardware-log analyzer."""

import tempfile
import unittest
from pathlib import Path

import analyze_c_ext_stress as analyzer


class CExtStressAnalyzerTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.path = Path(self.temporary.name) / "stress.log"

    def tearDown(self):
        self.temporary.cleanup()

    @staticmethod
    def _valid_lines():
        lines = [
            "I display_bench: c_ext_stress phase=begin sequence=1",
            "I display_bench: display config qspi_hz=40000000 draw_rows=60 "
            "color=RGB565 snapshot=enabled dma_rows=10 queue=2 direct=0 te=0 "
            "lv_os=none draw_units=1 draw_stack=0 adapter_stack=32768 "
            "load_profile=full lvgl_core=1 project_core=0",
            "I display_bench: c_ext_stress phase=load_start sequence=2",
            "I display_bench: c_ext_stress phase=provisioning_wait sequence=3",
            "I display_bench: c_ext_stress phase=warmup sequence=4",
            "I display_bench: c_ext_stress sample=1 phase=warmup "
            "internal_free=120000 internal_largest=60000 dma_free=80000 "
            "dma_largest=20000 psram_free=6000000 psram_largest=5900000 "
            "task_missing=0x0 task_internal=0x1e task_core_mismatch=0x0",
            "I display_bench: c_ext_stress phase=measure sequence=5",
            "I display_bench: c_ext_stress sample=2 phase=measure "
            "internal_free=110000 internal_largest=50000 dma_free=70000 "
            "dma_largest=18000 psram_free=5900000 psram_largest=5800000 "
            "task_missing=0x0 task_internal=0x1e task_core_mismatch=0x0",
            "I display_bench: display profile load=full diagnostics=PASS "
            "snapshot=enabled snapshot_fallbacks=0 min_dma=18000",
            "I display_bench: c_ext_stress phase=cleanup sequence=6",
            "W provisioning: BLE_HS_EDISABLED during active close",
            "I display_bench: c_ext_stress phase=end sequence=7",
        ]
        for name in sorted(analyzer.TASKS):
            hwm = 8192 if name == "lvgl" else 2048
            psram = int(name in analyzer.PSRAM_TASKS)
            lines.append(
                "I display_bench: c_ext_stress "
                f"task={name} result=PASS samples=3600 found=1 "
                f"stack_psram={psram} core={1 if name == 'lvgl' else 0} "
                f"expected_core={1 if name == 'lvgl' else 0} "
                f"core_match=1 min_hwm={hwm}"
            )
        lines.extend([
            "I display_bench: c_ext_stress ble result=PASS "
            "protected_success=900 protected_failure=0 snapshot_success=900 "
            "disconnects=0 reconnects=0 max_success_interval_us=2100000 "
            "max_success_idle_us=2200000",
            "I display_bench: c_ext_stress audio result=PASS "
            "tx_bytes=345600000 rx_bytes=345600000 target_bytes=345600000 "
            "tx_short=0 rx_short=0 tx_timeout=0 rx_timeout=0 "
            "tx_error=0 rx_error=0 tx_deadline_miss=0 rx_deadline_miss=0 "
            "faults=0 mic_nonzero=1",
            "I display_bench: c_ext_stress heap result=PASS "
            "min_internal_free=110000 min_internal_largest=50000 "
            "min_dma_free=70000 min_dma_largest=18000 "
            "min_psram_free=5900000 min_psram_largest=5800000 "
            "trend_ok=1 recovery_ok=1",
            "I display_bench: c_ext_stress summary result=PASS completed=1 "
            "navigation_ok=1 tasks_ok=1 performance=FLOOR",
            "I display_bench: display benchmark stability=PASS "
            "performance=FLOOR state=COMPLETE snapshot_fallbacks=0 "
            "snapshot_prepare_max_p95_us=80000",
            "I display_bench: display load profile=full tcp_rate_ok=1 "
            "tcp_reconnects=0 wifi_disconnects=0 workload=0x0 control=0x0 "
            "audio=0x0 tcp=0x0",
        ])
        return lines

    def _analyze(self, mutate=None):
        lines = self._valid_lines()
        if mutate is not None:
            mutate(lines)
        self.path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return analyzer.analyze(self.path)

    def test_valid_log_passes(self):
        self.assertEqual(self._analyze().status, "PASS")

    def test_duplicate_phase_is_invalid(self):
        def mutate(lines):
            lines.insert(1, lines[0])

        analysis = self._analyze(mutate)
        self.assertEqual(analysis.status, "INVALID")
        self.assertTrue(any("phase order" in item
                            for item in analysis.validation_errors))

    def test_zero_heap_minimum_fails(self):
        def mutate(lines):
            lines[5] = lines[5].replace(
                "internal_free=120000", "internal_free=0"
            )

        analysis = self._analyze(mutate)
        self.assertEqual(analysis.status, "FAIL")
        self.assertTrue(any("internal_free=0" in item
                            for item in analysis.gate_failures))

    def test_missing_task_sample_fails(self):
        def mutate(lines):
            lines[7] = lines[7].replace("task_missing=0x0", "task_missing=0x8")

        analysis = self._analyze(mutate)
        self.assertEqual(analysis.status, "FAIL")
        self.assertTrue(any("task_missing" in item
                            for item in analysis.gate_failures))

    def test_ble_disabled_during_measure_fails(self):
        def mutate(lines):
            lines.insert(8, "E nimble: BLE_HS_EDISABLED")

        analysis = self._analyze(mutate)
        self.assertEqual(analysis.status, "FAIL")
        self.assertTrue(any("outside cleanup" in item
                            for item in analysis.gate_failures))

    def test_fatal_marker_fails(self):
        def mutate(lines):
            lines.insert(8, "E system: Task watchdog got triggered")

        analysis = self._analyze(mutate)
        self.assertEqual(analysis.status, "FAIL")
        self.assertTrue(any("fatal marker" in item
                            for item in analysis.gate_failures))

    def test_directional_audio_fault_fails(self):
        def mutate(lines):
            for index, line in enumerate(lines):
                if "c_ext_stress audio " in line:
                    lines[index] = line.replace("rx_timeout=0", "rx_timeout=1")
                    break

        analysis = self._analyze(mutate)
        self.assertEqual(analysis.status, "FAIL")
        self.assertTrue(any("rx_timeout=1" in item
                            for item in analysis.gate_failures))


if __name__ == "__main__":
    unittest.main()
