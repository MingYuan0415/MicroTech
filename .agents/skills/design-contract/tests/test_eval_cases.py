#!/usr/bin/env python3
"""Check that design evaluation cases reference real profiles and targets."""

from __future__ import annotations

import importlib.util
import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).parents[4]
CASES = ROOT / "doc" / "design" / "evals" / "cases"
PROFILES = {path.stem for path in (ROOT / "doc" / "design" / "profiles").glob("*.json")}
TARGETS = {path.stem for path in (ROOT / "doc" / "design" / "targets").glob("*.json")}
SKILLS = {"design-contract", "visual-design", "aesthetic-review",
          "motion-interaction", "ux-heuristics"}
MATRIX = ROOT / "doc" / "design" / "evals" / "matrix.json"
DESIGN_SCRIPT = ROOT / ".agents" / "skills" / "design-contract" / "scripts" / "design_profile.py"
SPEC = importlib.util.spec_from_file_location("design_profile_eval", DESIGN_SCRIPT)
assert SPEC is not None and SPEC.loader is not None
DESIGN = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = DESIGN
SPEC.loader.exec_module(DESIGN)


class EvalCaseTest(unittest.TestCase):
    def _cases(self):
        return [json.loads(path.read_text(encoding="utf-8"))
                for path in sorted(CASES.glob("*.json"))]

    def test_cases_have_required_fields_and_real_references(self) -> None:
        cases = self._cases()
        self.assertGreaterEqual(len(cases), 12)
        ids = set()
        for case in cases:
            self.assertNotIn(case["id"], ids)
            ids.add(case["id"])
            self.assertIn(case["skill"], SKILLS)
            self.assertIn(case["profile"], PROFILES)
            self.assertIn(case["target"], TARGETS)
            self.assertTrue(case["brief"])
            self.assertTrue(case["must"])
            self.assertTrue(case["must_not"])

    def test_matrix_covers_every_profile_and_target(self) -> None:
        cases = self._cases()
        self.assertEqual({case["profile"] for case in cases}, PROFILES)
        self.assertEqual({case["target"] for case in cases}, TARGETS)

    def test_pairwise_matrix_is_complete(self) -> None:
        matrix = json.loads(MATRIX.read_text(encoding="utf-8"))
        self.assertEqual(set(matrix["styles"]), PROFILES)
        self.assertEqual(set(matrix["targets"]), TARGETS)
        self.assertEqual(len(matrix["runs"]), 30)
        self.assertEqual(len(matrix["styles"]) * len(matrix["targets"]), 30)
        self.assertEqual(
            {(run["style"], run["target"]) for run in matrix["runs"]},
            {(style, target)
             for style in matrix["styles"]
             for target in matrix["targets"]},
        )
        self.assertEqual({run["archetype"] for run in matrix["runs"]}, set(matrix["archetypes"]))
        self.assertTrue(all("expect" in run for run in matrix["runs"]))

    def test_matrix_runs_match_expected_resolution(self) -> None:
        matrix = json.loads(MATRIX.read_text(encoding="utf-8"))
        profiles = DESIGN.load_profiles(ROOT / "doc" / "design" / "profiles")
        targets = DESIGN.load_targets(ROOT / "doc" / "design" / "targets")
        for run in matrix["runs"]:
            expected = run["expect"]
            if expected in DESIGN.EXPECTED_PAIR_ERRORS:
                with self.assertRaises(DESIGN.DesignProfileError) as raised:
                    DESIGN.resolve(profiles[run["style"]], targets[run["target"]])
                self.assertEqual(raised.exception.code, expected, run["id"])
                continue
            context = DESIGN.resolve(profiles[run["style"]], targets[run["target"]])
            codes = {item["code"] for item in context["findings"]}
            if expected == "ok":
                self.assertEqual(codes, set(), run["id"])
            elif expected == "warning":
                self.assertEqual(codes, set(run["expect_codes"]), run["id"])
            else:
                self.fail(f"{run['id']}: unknown expect {expected!r}")

    def test_cases_resolve_or_declare_expected_error(self) -> None:
        profiles = DESIGN.load_profiles(ROOT / "doc" / "design" / "profiles")
        targets = DESIGN.load_targets(ROOT / "doc" / "design" / "targets")
        for case in self._cases():
            expected = case.get("expected_error")
            try:
                DESIGN.resolve(profiles[case["profile"]], targets[case["target"]])
            except DESIGN.DesignProfileError as error:
                self.assertEqual(error.code, expected, case["id"])
            else:
                self.assertIsNone(expected, case["id"])


    def test_neutral_fallback_case_uses_design_selection_mode(self) -> None:
        case = next(case for case in self._cases() if case["id"] == "neutral-fallback")
        self.assertEqual(case.get("selection_mode"), "design")
        context = DESIGN.select_profile(
            ROOT / "doc" / "design" / "profiles",
            ROOT / "doc" / "design" / "targets",
            active_path=ROOT / "doc" / "design" / "active.json",
            mode=case["selection_mode"],
        )
        self.assertEqual(context["profile"]["id"], case["profile"])
        self.assertEqual(context["target"]["id"], case["target"])
        self.assertEqual(context["selection"]["profile_source"], "fallback")

if __name__ == "__main__":
    unittest.main()
