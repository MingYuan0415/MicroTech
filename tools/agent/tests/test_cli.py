"""Command-line smoke tests that do not build or run repository suites."""

from __future__ import annotations

import json
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

    def test_stats_help_exposes_failure_flags(self):
        result = self._run("stats", "--help")
        self.assertEqual(result.returncode, 0, result.stdout)
        for flag in ("--failures", "--since", "--json"):
            self.assertIn(flag, result.stdout)

    def test_stats_failures_json_reads_isolated_stats(self):
        with tempfile.TemporaryDirectory() as tmp:
            stats = Path(tmp) / "stats.jsonl"
            stats.write_text(
                "\n".join(json.dumps(entry) for entry in [
                    {
                        "timestamp": "2026-11-01T10:00:00",
                        "command": "test",
                        "suite": "apps",
                        "rc": 0,
                        "failure_signature": None,
                        "failure_label": None,
                        "repeat_count": 0,
                    },
                    {
                        "timestamp": "2026-11-01T10:01:00",
                        "command": "test",
                        "suite": "apps",
                        "rc": 1,
                        "failure_signature": "abc",
                        "failure_label": "AssertionError: boom",
                        "repeat_count": 1,
                    },
                    {
                        "timestamp": "2026-11-01T10:02:00",
                        "command": "test",
                        "suite": "apps",
                        "rc": 1,
                        "failure_signature": "abc",
                        "failure_label": "AssertionError: boom",
                        "repeat_count": 2,
                    },
                    {
                        "timestamp": "2026-11-01T10:03:00",
                        "command": "test",
                        "suite": "apps",
                        "rc": 1,
                    },
                ]) + "\n",
                encoding="utf-8",
            )
            env = os.environ.copy()
            env["MT_STATS_PATH"] = str(stats)
            result = self._run("stats", "--failures", "--json", env=env)
        self.assertEqual(result.returncode, 0, result.stdout)
        payload = json.loads(result.stdout)
        self.assertEqual(payload["runs"], 4)
        self.assertEqual(payload["failures"], 3)
        self.assertEqual(payload["legacy"], 1)
        self.assertEqual(len(payload["groups"]), 2)
        self.assertEqual(payload["groups"][0]["count"], 2)
        self.assertEqual(payload["groups"][0]["signature"], "abc")

    def test_stats_rejects_invalid_since(self):
        with tempfile.TemporaryDirectory() as tmp:
            stats = Path(tmp) / "stats.jsonl"
            stats.write_text("{}\n", encoding="utf-8")
            env = os.environ.copy()
            env["MT_STATS_PATH"] = str(stats)
            result = self._run("stats", "--since", "not-a-date", env=env)
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn("--since must be YYYY-MM-DD", result.stdout)

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
