"""Command-line smoke tests that do not build or run repository suites."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve()
MT = HERE.parents[1] / "mt.py"


class CliTest(unittest.TestCase):
    def _run(self, *args: str, env: dict[str, str] | None = None):
        return subprocess.run(
            [sys.executable, str(MT), *args],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
            env=env,
        )

    def test_help(self):
        result = self._run("--help")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("build", result.stdout)
        self.assertIn("test", result.stdout)

    def test_sim_help_exposes_fast_flags(self):
        result = self._run("sim", "--help")
        self.assertEqual(result.returncode, 0, result.stdout)
        for flag in ("--no-build", "--jobs", "--keep-going"):
            self.assertIn(flag, result.stdout)

    def test_test_help_exposes_no_build(self):
        result = self._run("test", "--help")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("--no-build", result.stdout)

    def test_ui_help_exposes_state(self):
        result = self._run("ui", "--help")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("--state", result.stdout)

    def test_sim_no_build_rejects_product_mismatch(self):
        build = MT.parents[2] / "build/sim"
        if not (build / "microtech_sim").is_file():
            self.skipTest("build/sim is not available")
        env = os.environ.copy()
        env["SIM_PRODUCT"] = "p4_reference"
        result = self._run(
            "sim", "--no-build", "--scenario", "smoke_pages", env=env,
        )
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("configured for", result.stdout)

    def test_list_and_git_status_use_isolated_logs(self):
        with tempfile.TemporaryDirectory() as tmp:
            env = os.environ.copy()
            env["MT_LOG_ROOT"] = str(Path(tmp) / "logs")
            env["MT_STATS_PATH"] = str(Path(tmp) / "stats.jsonl")
            listed = self._run("list", env=env)
            self.assertEqual(listed.returncode, 0, listed.stdout)
            status = self._run("git", "status", env=env)
            self.assertEqual(status.returncode, 0, status.stdout)
            self.assertIn("log:", status.stdout)
            self.assertTrue(list((Path(tmp) / "logs").glob("*.log")))


if __name__ == "__main__":
    unittest.main()
