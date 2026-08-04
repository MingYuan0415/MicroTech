#!/usr/bin/env python3
"""Tests for the LVGL internal-RAM log analyzer."""

import tempfile
import unittest
from pathlib import Path

import analyze_lvgl_ram as analyzer
import lvgl_ram_profiles as profiles


class LvglRamAnalyzerTest(unittest.TestCase):
    def setUp(self):
        self._temporary_directory = tempfile.TemporaryDirectory()
        self.directory = Path(self._temporary_directory.name)

    def tearDown(self):
        self._temporary_directory.cleanup()

    def _write_log(
        self,
        name,
        *,
        minimum_internal_free,
        performance="FLOOR",
        stack_high_water=8192,
        task_override=None,
        psram_override=None,
        snapshot_fallbacks=0,
        snapshot_p95=80000,
        config_override=None,
        extra_line=None,
    ):
        profile = profiles.PROFILES[name]
        task_found = name != "B0" if task_override is None else task_override
        stack_psram = name == "C" if psram_override is None else psram_override
        if name == "B0":
            stack_high_water = 0
        config = {
            **analyzer.FIXED_CONFIG,
            **analyzer.FIXED_CONFIG_STRINGS,
            **analyzer._profile_config(profile),
        }
        if config_override:
            config.update(config_override)
        config_text = " ".join(
            f"{key}={value}" for key, value in config.items()
        )
        lines = [f"I (1) display_bench: display config {config_text}"]
        for load in analyzer.clock_analyzer.LOADS:
            for effect in analyzer.clock_analyzer.EFFECTS:
                lines.append(
                    "I (2) display_bench: display perf "
                    f"load={load} effect={effect} result=FLOOR "
                    "snapshot=enabled"
                )
            lines.append(
                "I (3) display_bench: display profile "
                f"load={load} diagnostics=PASS snapshot=enabled "
                "snapshot_prepare_count=20 snapshot_prepare_us=1000000 "
                "snapshot_prepare_max_us=90000 "
                f"snapshot_prepare_p95_us={snapshot_p95} "
                f"snapshot_fallbacks={snapshot_fallbacks} "
                "samples=150 min_dma=20000"
            )
            lines.append(
                "I (4) display_bench: display memory "
                f"load={load} min_internal_free={minimum_internal_free} "
                "min_internal_largest=50000 min_dma_free=80000 "
                "min_dma_largest=20000 min_psram_free=6000000 "
                "min_psram_largest=5900000 "
                f"render_task={int(task_found)} "
                f"render_stack_psram={int(stack_psram)} "
                f"render_stack_hwm={stack_high_water}"
            )
        lines.extend(
            [
                "I (5) display_bench: display benchmark stability=PASS "
                f"performance={performance} state=COMPLETE snapshot=enabled "
                "snapshot_prepare_count=40 snapshot_prepare_us=2000000 "
                "snapshot_prepare_max_us=90000 "
                f"snapshot_prepare_max_p95_us={snapshot_p95} "
                f"snapshot_fallbacks={snapshot_fallbacks} min_dma=20000",
                "I (6) display_bench: display memory summary "
                f"min_internal_free={minimum_internal_free} "
                "min_internal_largest=50000 min_dma_free=80000 "
                "min_dma_largest=20000 min_psram_free=6000000 "
                "min_psram_largest=5900000 "
                f"render_task={int(task_found)} "
                f"render_stack_psram={int(stack_psram)} "
                f"render_stack_hwm={stack_high_water}",
                "I (7) display_bench: display load profile=full "
                "tcp_rate_ok=1 tcp_reconnects=0 wifi_disconnects=0 "
                "workload=0x0 control=0x0 audio=0x0 tcp=0x0",
            ]
        )
        if extra_line:
            lines.append(extra_line)
        path = self.directory / f"{name.lower()}.log"
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return path

    def test_c_is_selected_when_all_primary_gates_pass(self):
        b0 = self._write_log("B0", minimum_internal_free=100000)
        c = self._write_log("C", minimum_internal_free=150000)

        analysis = analyzer.analyze((("B0", b0), ("C", c)))

        self.assertEqual(analysis.reports["B0"].status, "PASS")
        self.assertEqual(analysis.reports["C"].status, "PASS")
        self.assertEqual(analysis.reports["C"].internal_gain, 50000)
        self.assertEqual(analysis.selected, "C")

    def test_c_rejects_insufficient_measured_gain(self):
        b0 = self._write_log("B0", minimum_internal_free=100000)
        c = self._write_log("C", minimum_internal_free=149000)

        analysis = analyzer.analyze((("B0", b0), ("C", c)))

        self.assertEqual(analysis.reports["C"].status, "FAIL")
        self.assertTrue(any(
            "internal gain=49000" in reason
            for reason in analysis.reports["C"].gate_failures
        ))
        self.assertIsNone(analysis.selected)

    def test_failed_b0_can_still_supply_the_gain_baseline(self):
        b0 = self._write_log(
            "B0", minimum_internal_free=100000, performance="FAIL",
        )
        c = self._write_log("C", minimum_internal_free=150000)

        analysis = analyzer.analyze((("B0", b0), ("C", c)))

        self.assertEqual(analysis.reports["B0"].status, "FAIL")
        self.assertEqual(analysis.reports["C"].status, "PASS")
        self.assertEqual(analysis.reports["C"].internal_gain, 50000)
        self.assertEqual(analysis.selected, "C")

    def test_fallback_sequence_stops_after_stack_failure(self):
        paths = {
            "B0": self._write_log("B0", minimum_internal_free=100000),
            "C": self._write_log("C", minimum_internal_free=140000),
            "A": self._write_log("A", minimum_internal_free=130000),
            "B24": self._write_log("B24", minimum_internal_free=142000),
            "B20": self._write_log("B20", minimum_internal_free=146000),
            "B16": self._write_log(
                "B16", minimum_internal_free=150000,
                stack_high_water=3000,
            ),
        }

        analysis = analyzer.analyze(tuple(paths.items()))

        self.assertEqual(analysis.reports["B16"].status, "FAIL")
        self.assertEqual(analysis.selected, "B20")
        self.assertTrue(any(
            "stopped smaller stacks" in reason for reason in analysis.reasons
        ))

    def test_incomplete_fallback_sequence_does_not_select_early(self):
        b0 = self._write_log("B0", minimum_internal_free=100000)
        c = self._write_log("C", minimum_internal_free=140000)
        a = self._write_log("A", minimum_internal_free=130000)

        analysis = analyzer.analyze((
            ("B0", b0), ("C", c), ("A", a),
        ))

        self.assertEqual(analysis.reports["A"].status, "PASS")
        self.assertIsNone(analysis.selected)
        self.assertIn(
            "missing B24 log; stopped fallback sequence", analysis.reasons
        )

    def test_invalid_c_does_not_start_fallback_evaluation(self):
        b0 = self._write_log("B0", minimum_internal_free=100000)
        c = self._write_log(
            "C", minimum_internal_free=160000,
            config_override={"draw_rows": 120},
        )
        a = self._write_log("A", minimum_internal_free=130000)

        analysis = analyzer.analyze((
            ("B0", b0), ("C", c), ("A", a),
        ))

        self.assertEqual(analysis.reports["C"].status, "INVALID")
        self.assertIsNone(analysis.selected)
        self.assertIn(
            "C log is invalid; fallback evaluation cannot start",
            analysis.reasons,
        )

    def test_c_requires_psram_stack_and_rejects_adapter_fallback(self):
        path = self._write_log(
            "C", minimum_internal_free=160000, psram_override=False,
            extra_line=(
                "W esp_lv_adapter: LVGL task PSRAM allocation failed, "
                "retrying with internal memory"
            ),
        )

        report = analyzer.parse_log("C", path)

        self.assertEqual(report.status, "FAIL")
        self.assertTrue(any(
            "render_stack_psram=0" in reason
            for reason in report.gate_failures
        ))
        self.assertTrue(any(
            "psram allocation failed" in reason
            for reason in report.gate_failures
        ))

    def test_snapshot_and_performance_gates_are_enforced(self):
        path = self._write_log(
            "A", minimum_internal_free=140000, performance="FAIL",
            snapshot_fallbacks=1, snapshot_p95=110000,
        )

        report = analyzer.parse_log("A", path)

        self.assertEqual(report.status, "FAIL")
        self.assertTrue(any(
            "performance=FAIL" in reason for reason in report.gate_failures
        ))
        self.assertTrue(any(
            "snapshot_fallbacks=1" in reason
            for reason in report.gate_failures
        ))
        self.assertTrue(any(
            "snapshot_prepare_p95_us=110000" in reason
            for reason in report.gate_failures
        ))

    def test_profile_fingerprint_mismatch_is_invalid(self):
        path = self._write_log(
            "C", minimum_internal_free=160000,
            config_override={"draw_rows": 120},
        )

        report = analyzer.parse_log("C", path)

        self.assertEqual(report.status, "INVALID")
        self.assertTrue(any(
            "draw_rows=120" in reason
            for reason in report.validation_errors
        ))

    def test_summary_task_mismatch_fails_candidate(self):
        path = self._write_log("C", minimum_internal_free=160000)
        lines = path.read_text(encoding="utf-8").splitlines()
        lines[-2] = lines[-2].replace("render_task=1", "render_task=0")
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")

        report = analyzer.parse_log("C", path)

        self.assertEqual(report.status, "FAIL")
        self.assertTrue(any(
            "memory summary: render_task=0" in reason
            for reason in report.gate_failures
        ))

    def test_duplicate_profile_logs_do_not_select_candidate(self):
        b0 = self._write_log("B0", minimum_internal_free=100000)
        c = self._write_log("C", minimum_internal_free=160000)

        analysis = analyzer.analyze(
            (("B0", b0), ("C", c), ("C", c))
        )

        self.assertIsNone(analysis.selected)
        self.assertIn("duplicate log for C", analysis.reasons)


if __name__ == "__main__":
    unittest.main()
