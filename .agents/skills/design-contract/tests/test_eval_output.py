#!/usr/bin/env python3
"""Tests for the saved-output evaluator."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).parents[4]
SCRIPT = ROOT / "doc" / "design" / "evals" / "check_output.py"
SPEC = importlib.util.spec_from_file_location("check_eval_output", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
OUTPUT = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = OUTPUT
SPEC.loader.exec_module(OUTPUT)


class EvalOutputTest(unittest.TestCase):
    def test_must_and_must_not_are_enforced(self) -> None:
        case = {
            "id": "x",
            "must": ["profile_id:neutral"],
            "must_not": ["dark_sparse"],
        }
        self.assertTrue(OUTPUT.check_output(case, "profile_id:neutral")["ok"])
        self.assertFalse(OUTPUT.check_output(case, "profile_id:neutral; dark_sparse")["ok"])

    def test_critical_current_ui_literals_are_rejected(self) -> None:
        case = {"id": "x", "must": ["profile_id:neutral"], "must_not": []}
        result = OUTPUT.check_output(case, "profile_id:neutral #FF00AA")
        self.assertFalse(result["ok"])
        self.assertTrue(result["critical"])

    def test_bare_dimensions_are_rejected(self) -> None:
        case = {"id": "x", "must": [], "must_not": []}
        for output in ("width 368", "use 448 px"):
            result = OUTPUT.check_output(case, output)
            self.assertFalse(result["ok"], output)
            self.assertTrue(result["critical"], output)

    def test_target_id_dimensions_are_allowed(self) -> None:
        case = {
            "id": "x",
            "must": ["target_id:panel_368x448_touch_60hz"],
            "must_not": [],
        }
        result = OUTPUT.check_output(
            case, "target_id:panel_368x448_touch_60hz")
        self.assertTrue(result["ok"])
        self.assertEqual(result["critical"], [])


if __name__ == "__main__":
    unittest.main()
