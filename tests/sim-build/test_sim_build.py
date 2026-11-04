"""Simulator build-gate test used by the repository Python test entry."""

from __future__ import annotations

import os
import subprocess
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class SimBuildGateTest(unittest.TestCase):
    def test_simulator_build_gate(self):
        if not os.environ.get("IDF_PATH"):
            self.skipTest("IDF_PATH is unset")
        if not (ROOT / "build/config/sdkconfig.h").is_file():
            self.skipTest("build/config/sdkconfig.h is missing")
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools/agent/mt.py"),
             "build", "sim"],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout[-4000:])


if __name__ == "__main__":
    unittest.main()
