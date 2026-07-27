#!/usr/bin/env python3
"""Tests for the 40/80 MHz display log analyzer."""

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import analyze_clock_ab as analyzer


class ClockAbAnalyzerTest(unittest.TestCase):
    def setUp(self):
        self._temporary_directory = tempfile.TemporaryDirectory()
        self.directory = Path(self._temporary_directory.name)

    def tearDown(self):
        self._temporary_directory.cleanup()

    def _write_log(
        self,
        name,
        label,
        *,
        p95_us=100_000,
        fps_x100=1_000,
        panel_frame_us=16_000,
        minimum_dma=20_000,
        dma_fail=0,
        effect_overrides=None,
        config_clock_hz=None,
        tcp_rate_ok=1,
        omit=None,
    ):
        spec = analyzer.PROFILE_SPECS[label]
        minimum_by_load = {
            "display-only": minimum_dma + 1000,
            "full": minimum_dma,
        }
        failures_by_load = {"display-only": 0, "full": dma_fail}
        lines = [
            "I (1) board_display: LCD SPI "
            f"clock_hz={spec.clock_hz} max_lines=10 queue=2 direct=0 te=0",
            f"I (2) app_adapter: display color format: {spec.color}",
            "I (3) display_bench: display config "
            f"qspi_hz={config_clock_hz or spec.clock_hz} "
            f"draw_rows={spec.draw_rows} color={spec.color} dma_rows=10 "
            "dma_max_full_rows=44 queue=2 direct=0 te=0 draw_units=2 "
            "draw_prio=3 tcp_payload=5760 tcp_prio=2 load_profile=full "
            "lifecycle_log=0",
        ]
        effect_overrides = effect_overrides or {}
        active_frames = 100
        for load in analyzer.LOADS:
            for index, effect in enumerate(analyzer.EFFECTS):
                key = (load, effect)
                values = {
                    "p95_us": p95_us + index * 1000,
                    "fps_x100": fps_x100 + index,
                    "panel_frame_us": panel_frame_us,
                }
                values.update(effect_overrides.get(key, {}))
                lines.append(
                    "I (4) display_bench: display perf "
                    f"load={load} effect={effect} result=FAIL start=20 "
                    "complete=20 cancel=0 active_frames=100 active_ms=10000 "
                    f"avg_fps_x100={values['fps_x100']} intervals=80 "
                    "target_pct_x100=0 p50_us=80000 "
                    f"p95_us={values['p95_us']} p99_us=120000 max_us=130000 "
                    "long_run=2"
                )
                lines.append(
                    "I (5) display_bench: display cost "
                    f"load={load} effect={effect} render_count=100 "
                    "render_us=7000000 render_max_us=80000 flush_count=400 "
                    "flush_pixels=1000000 flush_us=1000000 flush_max_us=3000 "
                    "flush_wait_count=400 flush_wait_us=10000 "
                    "flush_wait_max_us=100 panel_count=400 panel_pixels=1000000 "
                    f"panel_us={values['panel_frame_us'] * active_frames} "
                    "panel_max_us=3000"
                )
            diagnostics = (
                "PASS"
                if minimum_by_load[load] >= analyzer.DMA_LARGEST_MINIMUM
                and failures_by_load[load] == 0
                else "FAIL"
            )
            lines.append(
                "I (6) display_bench: display profile "
                f"load={load} diagnostics={diagnostics} samples=150 "
                "sample_err=0 lock_err=0 fps_read_err=0 fps_lock_max_us=10 "
                "min_fps=9 fps_below_30=150 "
                f"min_dma={minimum_by_load[load]} "
                f"dma_fail={failures_by_load[load]} frame_submits=500 "
                "panel_submits=2000 submit_fail=0"
            )
        stability = (
            "PASS"
            if minimum_dma >= analyzer.DMA_LARGEST_MINIMUM
            and dma_fail == 0
            and tcp_rate_ok == 1
            else "FAIL"
        )
        lines.extend(
            [
                "I (7) display_bench: display benchmark "
                f"stability={stability} performance=FAIL state=COMPLETE "
                "samples=300 sample_err=0 lock_err=0 fps_read_err=0 "
                "fps_lock_max_us=10 "
                f"min_dma={minimum_dma} dma_fail={dma_fail} "
                "frame_submits=1000 submit_fail=0 transition_cancel=0",
                "I (8) display_bench: display load profile=full tcp_required=1 "
                "tcp_tx_bytes=1000000 tcp_rx_bytes=1000000 "
                "tcp_target_bytes=1000000 tcp_active_us=1000000 "
                f"tcp_rate_ok={tcp_rate_ok} tcp_reconnects=0 tcp_down_ms=20 "
                "tcp_pacing_late=0 tcp_pacing_max_lag_us=0 "
                "wifi_disconnects=0 workload=0x0 control=0x0 audio=0x0 tcp=0x0",
            ]
        )
        if omit is not None:
            lines = [line for line in lines if omit not in line]
        path = self.directory / name
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return path

    def test_dma_failure_is_separate_from_transport_result(self):
        entries = []
        for index in range(2):
            entries.append(
                (
                    "E40",
                    self._write_log(
                        f"e40-{index}.log",
                        "E40",
                        p95_us=100_000 + index * 2_000,
                        panel_frame_us=16_000,
                        minimum_dma=8192,
                        dma_fail=4,
                    ),
                )
            )
            entries.append(
                (
                    "E80",
                    self._write_log(
                        f"e80-{index}.log",
                        "E80",
                        p95_us=88_000 + index * 2_000,
                        panel_frame_us=8_000,
                        minimum_dma=8192,
                        dma_fail=4,
                    ),
                )
            )

        aggregates, comparisons = analyzer.analyze(entries)

        self.assertEqual(len(aggregates["E40"].runs), 2)
        self.assertEqual(aggregates["E40"].worst_average_p95_us, 105_000)
        self.assertTrue(aggregates["E40"].transport_passed)
        self.assertFalse(aggregates["E40"].system_stability_passed)
        self.assertEqual(comparisons[0].status, "PASS")
        self.assertTrue(comparisons[0].transport_passed)
        self.assertFalse(comparisons[0].system_stability_passed)
        self.assertAlmostEqual(comparisons[0].panel_reduction_percent, 50.0)

    def test_candidate_only_dma_failure_rejects_80_mhz(self):
        entries = []
        for index in range(2):
            entries.append((
                "E40",
                self._write_log(
                    f"e40-stable-{index}.log",
                    "E40",
                    p95_us=100_000,
                    panel_frame_us=16_000,
                    minimum_dma=20_000,
                ),
            ))
            entries.append((
                "E80",
                self._write_log(
                    f"e80-dma-fail-{index}.log",
                    "E80",
                    p95_us=85_000,
                    panel_frame_us=8_000,
                    minimum_dma=8192,
                    dma_fail=4,
                ),
            ))

        _, comparisons = analyzer.analyze(entries)

        self.assertEqual(comparisons[0].status, "FAIL")
        self.assertTrue(any(
            "introduced a DMA/internal" in reason
            for reason in comparisons[0].reasons
        ))

    def test_all_three_transport_thresholds_are_enforced(self):
        baselines = [
            self._write_log(f"e40-{index}.log", "E40")
            for index in range(2)
        ]
        candidates = [
            self._write_log(
                f"e80-{index}.log",
                "E80",
                p95_us=96_000,
                panel_frame_us=10_600,
                effect_overrides={
                    ("full", "fade"): {"fps_x100": 940},
                },
            )
            for index in range(2)
        ]

        _, comparisons = analyzer.analyze(
            [("E40", path) for path in baselines]
            + [("E80", path) for path in candidates]
        )

        comparison = comparisons[0]
        self.assertEqual(comparison.status, "FAIL")
        self.assertEqual(len(comparison.fps_regressions), 1)
        self.assertTrue(any("P95" in reason for reason in comparison.reasons))
        self.assertTrue(any("panel cost" in reason for reason in comparison.reasons))
        self.assertTrue(any("average FPS" in reason for reason in comparison.reasons))
        self.assertFalse(any("expected 2" in reason for reason in comparison.reasons))

    def test_fingerprint_mismatch_and_missing_effect_are_invalid(self):
        bad_clock = self._write_log(
            "bad-clock.log", "E80", config_clock_hz=40_000_000
        )
        missing_effect = self._write_log(
            "missing-effect.log",
            "E40",
            omit="load=full effect=reveal-right",
        )

        clock_report = analyzer.parse_log("E80", bad_clock)
        effect_report = analyzer.parse_log("E40", missing_effect)

        self.assertFalse(clock_report.valid)
        self.assertTrue(
            any("qspi_hz=40000000" in error for error in clock_report.validation_errors)
        )
        self.assertFalse(effect_report.valid)
        self.assertTrue(
            any(
                "full/reveal-right" in error
                for error in effect_report.validation_errors
            )
        )

    def test_duplicate_startup_fingerprints_are_invalid(self):
        markers = (
            "board_display: LCD SPI",
            "app_adapter: display color format:",
            "display_bench: display config",
        )
        for index, marker in enumerate(markers):
            with self.subTest(marker=marker):
                path = self._write_log(f"duplicate-{index}.log", "E40")
                lines = path.read_text(encoding="utf-8").splitlines()
                duplicate = next(line for line in lines if marker in line)
                path.write_text(
                    "\n".join([duplicate, *lines]) + "\n", encoding="utf-8"
                )

                report = analyzer.parse_log("E40", path)

                self.assertFalse(report.valid)
                self.assertTrue(
                    any("expected one" in error for error in report.validation_errors)
                )

    def test_sample_counts_must_cover_both_full_profiles(self):
        path = self._write_log("bad-samples.log", "E40")
        contents = path.read_text(encoding="utf-8")
        contents = contents.replace("samples=150", "samples=149", 1)
        contents = contents.replace("samples=300", "samples=299", 1)
        path.write_text(contents, encoding="utf-8")

        report = analyzer.parse_log("E40", path)

        self.assertFalse(report.transport_passed)
        self.assertTrue(any(
            "samples=149, expected 150" in error
            for error in report.transport_errors
        ))
        self.assertTrue(any(
            "samples=299, expected 300" in error
            for error in report.transport_errors
        ))

    def test_duplicate_input_path_cannot_satisfy_run_count(self):
        e40 = self._write_log("e40.log", "E40")
        e80_first = self._write_log("e80-1.log", "E80")
        e80_second = self._write_log("e80-2.log", "E80")

        aggregates, comparisons = analyzer.analyze([
            ("E40", e40),
            ("E40", e40),
            ("E80", e80_first),
            ("E80", e80_second),
        ])

        self.assertFalse(aggregates["E40"].valid)
        self.assertEqual(comparisons[0].status, "INVALID")
        self.assertTrue(any(
            "duplicate input log path" in error
            for run in aggregates["E40"].runs
            for error in run.validation_errors
        ))

    def test_stack_overflow_and_repeated_boot_are_runtime_failures(self):
        path = self._write_log("fatal.log", "E80")
        with path.open("a", encoding="utf-8") as output:
            output.write("boot: ESP-IDF v6.0.2 2nd stage bootloader\n")
            output.write("boot: ESP-IDF v6.0.2 2nd stage bootloader\n")
            output.write("***ERROR*** A stack overflow in task display_tcp\n")

        report = analyzer.parse_log("E80", path)

        self.assertFalse(report.transport_passed)
        self.assertTrue(any(
            "stack overflow" in error for error in report.transport_errors
        ))
        self.assertTrue(any(
            "repeated startup marker" in error
            for error in report.transport_errors
        ))

    def test_experimental_pair_requires_two_runs_per_clock(self):
        e40 = self._write_log("one-e40.log", "E40")
        e80 = self._write_log(
            "one-e80.log", "E80", p95_us=85_000, panel_frame_us=8_000
        )

        aggregates, comparisons = analyzer.analyze(
            [("E40", e40), ("E80", e80)]
        )

        self.assertIsNotNone(aggregates["E40"].worst_average_p95_us)
        self.assertEqual(comparisons[0].status, "FAIL")
        self.assertTrue(
            any("E40 has 1 runs, expected 2" in reason
                for reason in comparisons[0].reasons)
        )
        self.assertTrue(
            any("E80 has 1 runs, expected 2" in reason
                for reason in comparisons[0].reasons)
        )

    def test_tcp_failure_invalidates_transport(self):
        report = analyzer.parse_log(
            "E80",
            self._write_log("tcp-fail.log", "E80", tcp_rate_ok=0),
        )

        self.assertTrue(report.valid)
        self.assertFalse(report.transport_passed)
        self.assertTrue(any("tcp_rate_ok=0" in error for error in report.transport_errors))

    def test_json_cli_and_unmatched_profile_exit_codes(self):
        p40 = self._write_log("p40.log", "P40")
        p80 = self._write_log(
            "p80.log", "P80", p95_us=85_000, panel_frame_us=8_000
        )
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            result = analyzer.main(
                ["--log", f"P40={p40}", "--log", f"P80={p80}", "--json"]
            )

        parsed = json.loads(output.getvalue())
        self.assertEqual(result, 0)
        self.assertEqual(parsed["comparisons"][0]["status"], "PASS")
        self.assertEqual(parsed["unmatched_profiles"], [])

        with contextlib.redirect_stdout(io.StringIO()):
            result = analyzer.main(["--log", f"P40={p40}"])
        self.assertEqual(result, 1)


if __name__ == "__main__":
    unittest.main()
