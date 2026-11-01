#!/usr/bin/env python3
"""Tests for the embedded motion budget validator."""

from __future__ import annotations

import importlib.util
import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).parents[4]
PROFILE_SCRIPT = ROOT / ".agents" / "skills" / "design-contract" / "scripts" / "design_profile.py"
BUDGET_SCRIPT = Path(__file__).parents[1] / "scripts" / "motion_budget.py"


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


DESIGN = _load("design_profile_motion", PROFILE_SCRIPT)
BUDGET = _load("motion_budget", BUDGET_SCRIPT)
PROFILES = ROOT / "doc" / "design" / "profiles"
TARGETS = ROOT / "doc" / "design" / "targets"


def _context(profile: str = "dark_sparse", target: str = "panel_368x448_touch_60hz") -> dict:
    profile_data = json.loads((PROFILES / f"{profile}.json").read_text(encoding="utf-8"))
    target_data = json.loads((TARGETS / f"{target}.json").read_text(encoding="utf-8"))
    return DESIGN.resolve(profile_data, target_data)


def _codes(result: dict) -> set[str]:
    return {finding["code"] for finding in result["findings"]}


class MotionBudgetTest(unittest.TestCase):
    def test_clean_micro_plan_passes(self) -> None:
        plan = {
            "profile_id": "dark_sparse",
            "target_id": "panel_240x320_touch_30hz",
            "items": [{
                "id": "button-press", "trigger": "press", "property": "opacity",
                "duration_ms": 120, "objects": 1, "loop": False,
                "mechanism": "style_transition",
            }],
        }
        result = BUDGET.validate_plan(plan, _context("dark_sparse", "panel_240x320_touch_30hz"))
        self.assertEqual(result["findings"], [])

    def test_version_pins_must_match_context(self) -> None:
        case = {
            "profile_id": "dark_sparse",
            "target_id": "panel_240x320_touch_30hz",
            "items": [],
        }
        for key, code in (("profile_version", "PROFILE_VERSION_MISMATCH"),
                          ("target_version", "TARGET_VERSION_MISMATCH")):
            plan = dict(case)
            plan[key] = 99
            result = BUDGET.validate_plan(
                plan, _context("dark_sparse", "panel_240x320_touch_30hz"))
            self.assertIn(code, _codes(result), key)

    def test_invalid_version_pin_is_rejected(self) -> None:
        plan = {"profile_version": True, "items": []}
        result = BUDGET.validate_plan(plan, _context())
        self.assertIn("INVALID_PROFILE_VERSION", _codes(result))

    def test_duration_over_budget(self) -> None:
        plan = {"items": [{"id": "page-enter", "trigger": "navigate", "property": "translate_y",
                           "duration_ms": 300, "objects": 1, "loop": False,
                           "mechanism": "lv_anim"}]}
        result = BUDGET.validate_plan(plan, _context("dark_sparse", "panel_240x320_touch_30hz"))
        self.assertIn("DURATION_EXCEEDS_BUDGET", _codes(result))

    def test_too_many_objects(self) -> None:
        plan = {"items": [{"id": "stagger", "trigger": "navigate", "property": "translate_y",
                           "duration_ms": 100, "objects": 5, "loop": False,
                           "mechanism": "lv_anim"}]}
        result = BUDGET.validate_plan(plan, _context("dark_sparse", "panel_240x320_touch_30hz"))
        self.assertIn("TOO_MANY_OBJECTS", _codes(result))

    def test_cumulative_objects_are_limited(self) -> None:
        plan = {"items": [
            {"id": str(index), "trigger": "navigate", "property": "opacity",
             "duration_ms": 100, "objects": 2, "loop": False,
             "mechanism": "style_transition"}
            for index in range(3)
        ]}
        result = BUDGET.validate_plan(
            plan, _context("dark_sparse", "panel_240x320_touch_30hz"))
        self.assertIn("TOTAL_OBJECTS_EXCEEDS_BUDGET", _codes(result))

    def test_none_target_accepts_instant_plan(self) -> None:
        plan = {"items": [{"id": "press", "trigger": "press",
                           "duration_ms": 0, "objects": 0, "loop": False,
                           "mechanism": "none"}]}
        result = BUDGET.validate_plan(plan, _context("mono_industrial", "panel_128x64_mono_keypad"))
        self.assertEqual(result["findings"], [])

    def test_none_target_rejects_animated_plan(self) -> None:
        plan = {"items": [{"id": "press", "trigger": "press", "property": "opacity",
                           "duration_ms": 100, "objects": 1, "loop": False,
                           "mechanism": "style_transition"}]}
        result = BUDGET.validate_plan(plan, _context("mono_industrial", "panel_128x64_mono_keypad"))
        self.assertIn("MOTION_NOT_ALLOWED", _codes(result))

    def test_instant_plan_cannot_have_nonzero_duration_or_objects(self) -> None:
        plan = {"items": [{"id": "press", "trigger": "press",
                           "duration_ms": 100, "objects": 1, "loop": False,
                           "mechanism": "none"}]}
        result = BUDGET.validate_plan(plan, _context())
        self.assertIn("INSTANT_DURATION_NONZERO", _codes(result))
        self.assertIn("INSTANT_OBJECTS_NONZERO", _codes(result))

    def test_keypad_and_list_are_high_frequency(self) -> None:
        for trigger in ("keypad", "list", "list_navigation"):
            plan = {"items": [{"id": trigger, "trigger": trigger,
                               "property": "opacity", "duration_ms": 100,
                               "objects": 1, "loop": False,
                               "mechanism": "style_transition"}]}
            result = BUDGET.validate_plan(plan, _context())
            self.assertIn("HIGH_FREQUENCY_ANIMATION", _codes(result), trigger)

    def test_layout_property_is_rejected(self) -> None:
        plan = {"items": [{"id": "grow", "trigger": "navigate", "property": "width",
                           "duration_ms": 100, "objects": 1, "loop": False,
                           "mechanism": "lv_anim"}]}
        result = BUDGET.validate_plan(plan, _context())
        self.assertIn("PROPERTY_NOT_ALLOWED", _codes(result))

    def test_loop_is_rejected(self) -> None:
        plan = {"items": [{"id": "pulse", "trigger": "idle", "property": "opacity",
                           "duration_ms": 100, "objects": 1, "loop": True,
                           "mechanism": "lv_anim"}]}
        result = BUDGET.validate_plan(plan, _context())
        self.assertIn("LOOP_NOT_ALLOWED", _codes(result))

    def test_high_frequency_back_must_be_instant(self) -> None:
        plan = {"items": [{"id": "back", "trigger": "back", "property": "translate_x",
                           "duration_ms": 100, "objects": 1, "loop": False,
                           "mechanism": "lv_anim"}]}
        result = BUDGET.validate_plan(plan, _context())
        self.assertIn("HIGH_FREQUENCY_ANIMATION", _codes(result))

    def test_blur_and_shadow_properties_are_rejected(self) -> None:
        for prop in ("blur", "shadow"):
            plan = {"items": [{"id": prop, "trigger": "navigate", "property": prop,
                               "duration_ms": 100, "objects": 1, "loop": False,
                               "mechanism": "lv_anim"}]}
            result = BUDGET.validate_plan(plan, _context())
            self.assertIn("PROPERTY_NOT_ALLOWED", _codes(result))


if __name__ == "__main__":
    unittest.main()
