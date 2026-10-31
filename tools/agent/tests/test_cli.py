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
