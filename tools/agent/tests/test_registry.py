"""Registry and command-line smoke tests for the agent wrapper."""

from __future__ import annotations

import subprocess
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve()
sys.path.insert(0, str(HERE.parents[1]))

import mt  # noqa: E402


class RegistryTest(unittest.TestCase):
    def test_known_suites_are_present(self):
        for alias in ("main", "bsp", "apps", "app-core", "integration",
                      "connectivity", "weather-service", "recorder-service"):
            self.assertIn(alias, mt.SUITES, alias)

    def test_every_suite_has_a_cmake_project(self):
        self.assertGreaterEqual(len(mt.SUITES), 23)
        for suite in mt.SUITES.values():
            self.assertTrue((suite.source / "CMakeLists.txt").is_file(),
                            suite.alias)
            self.assertEqual(suite.alias, suite.alias.strip())

    def test_sanitizer_variables_are_real(self):
        for suite in mt.SUITES.values():
            if suite.sanitizer_var is None:
                continue
            text = (suite.source / "CMakeLists.txt").read_text(
                encoding="utf-8", errors="replace")
            self.assertIn(suite.sanitizer_var, text, suite.alias)

    def test_connectivity_is_tagged_needs_idf(self):
        self.assertIn("needs-idf", mt.SUITES["connectivity"].tags)

    def test_list_command_runs(self):
        result = subprocess.run(
            [sys.executable, str(HERE.parents[1] / "mt.py"), "list"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("apps", result.stdout)
        self.assertIn("weather", result.stdout)


if __name__ == "__main__":
    unittest.main()
