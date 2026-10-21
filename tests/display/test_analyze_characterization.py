"""Characterization must observe all ten effect/load windows."""
import tempfile
import unittest
from pathlib import Path

import analyze_characterization as analyzer
# Import the module, not the class: binding a TestCase in this module's
# namespace makes unittest discover collect it a second time.
import test_analyze_c_ext_stress as stress_fixture


class CharacterizationTest(unittest.TestCase):
    def test_effect_matrix_and_numeric_floor(self):
        lines = stress_fixture.CExtStressAnalyzerTest._valid_lines()
        lines = [line for line in lines if "display profile " not in line]
        for load in ("display-only", "full"):
            for effect in ("fade", "push-left", "push-right", "cover-left", "reveal-right"):
                lines.append(f"display perf load={load} effect={effect} result=PASS "
                             "active_frames=100 active_ms=3000 intervals=99 complete=1 "
                             "snapshot_prepare_count=1 cancel=0 snapshot_fallbacks=0 "
                             "p95_us=33000 p99_us=40000 max_us=50000 "
                             "snapshot_prepare_p95_us=50000 avg_fps_x100=3300 snapshot=enabled")
            lines.append(f"display profile load={load} diagnostics=PASS samples=2 "
                         "sample_err=0 submit_fail=0 dma_fail=0 snapshot_fallbacks=0")
            lines.append(f"display memory load={load} min_internal_free=120000 "
                         "min_internal_largest=60000 min_dma_free=60000 min_dma_largest=20000 "
                         "min_psram_free=5900000 min_psram_largest=5800000 "
                         "render_task=1 render_stack_psram=1 render_core=1 render_stack_hwm=8192")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "log"
            path.write_text("\n".join(lines))
            self.assertEqual(analyzer.analyze(path).status, "PASS")
            path.write_text("\n".join(lines).replace("avg_fps_x100=3300", "avg_fps_x100=100"))
            self.assertEqual(analyzer.analyze(path).status, "FAIL")
            path.write_text("\n".join(line for line in lines if "effect=fade" not in line))
            self.assertEqual(analyzer.analyze(path).status, "INVALID")
