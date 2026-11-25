"""Simulator build-gate test used by the repository Python test entry."""

from __future__ import annotations

import os
import subprocess
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class SimBuildGateTest(unittest.TestCase):
    def test_run_ci_help_exposes_fast_flags(self):
        result = subprocess.run(
            ["sh", str(ROOT / "sim/ci/run_ci.sh"), "--help"],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        for flag in ("--no-build", "--jobs", "--keep-going"):
            self.assertIn(flag, result.stdout)

    def test_run_ci_rejects_unknown_flag(self):
        result = subprocess.run(
            ["sh", str(ROOT / "sim/ci/run_ci.sh"), "--not-a-flag"],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        self.assertNotEqual(result.returncode, 0, result.stdout)

    def test_run_ci_no_build_rejects_product_mismatch(self):
        build = ROOT / "build/sim"
        if not (build / "microtech_sim").is_file():
            self.skipTest("build/sim is not available")
        env = os.environ.copy()
        env["SIM_PRODUCT"] = "p4_reference"
        result = subprocess.run(
            ["sh", str(ROOT / "sim/ci/run_ci.sh"), "--no-build",
             "--scenario", "smoke_pages"],
            cwd=ROOT,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("configured for", result.stdout)

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
