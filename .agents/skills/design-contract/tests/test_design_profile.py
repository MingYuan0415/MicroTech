#!/usr/bin/env python3
"""Tests for the design-contract profile resolver and validator."""

from __future__ import annotations

import contextlib
import copy
import importlib.util
import io
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).parents[1] / "scripts" / "design_profile.py"
SPEC = importlib.util.spec_from_file_location("design_profile", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
DESIGN = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = DESIGN
SPEC.loader.exec_module(DESIGN)

ROOT = Path(__file__).parents[4]
PROFILES_DIR = ROOT / "doc" / "design" / "profiles"
TARGETS_DIR = ROOT / "doc" / "design" / "targets"


def _profile(name: str) -> dict:
    return json.loads((PROFILES_DIR / f"{name}.json").read_text(encoding="utf-8"))


def _target(name: str) -> dict:
    return json.loads((TARGETS_DIR / f"{name}.json").read_text(encoding="utf-8"))


class DesignProfileTest(unittest.TestCase):
    def test_all_profiles_and_targets_validate(self) -> None:
        for profile in DESIGN.load_profiles(PROFILES_DIR).values():
            self.assertEqual(DESIGN.validate_profile(profile), [], profile["id"])
        for target in DESIGN.load_targets(TARGETS_DIR).values():
            self.assertEqual(DESIGN.validate_target(target), [], target["id"])

    def test_current_style_resolves_against_current_target(self) -> None:
        context = DESIGN.resolve(_profile("dark_sparse"),
                                 _target("panel_368x448_touch_60hz"))
        resolved = context["resolved"]
        self.assertEqual(resolved["motion_level"], "micro")
        self.assertEqual(resolved["max_motion_ms"], 150)
        self.assertEqual(resolved["max_animated_objects"], 2)
        self.assertEqual(resolved["type_px"]["body"], 24)
        self.assertEqual(resolved["type_px"]["hero"], 96)
        self.assertEqual(resolved["space_px"], [4, 8, 12, 16, 24, 32, 48])
        self.assertEqual(resolved["radius_px"], [0, 12, 20, 28])
        self.assertEqual(resolved["min_touch_px"], 52)
        self.assertFalse(resolved["allow_shadow"])
        self.assertFalse(resolved["allow_blur"])
        self.assertEqual(resolved["min_contrast_body"], 4.5)
        self.assertEqual(resolved["min_contrast_faint"], 4.0)

    def test_soft_style_is_downgraded_on_constrained_target(self) -> None:
        context = DESIGN.resolve(_profile("soft_playful"),
                                 _target("panel_240x320_touch_30hz"))
        resolved = context["resolved"]
        self.assertEqual(resolved["motion_level"], "micro")
        self.assertFalse(resolved["allow_shadow"])
        self.assertFalse(resolved["allow_gradients"])
        codes = {finding["code"] for finding in context["findings"]}
        self.assertIn("MOTION_DOWNGRADED", codes)
        self.assertIn("EFFECT_DOWNGRADED", codes)

    def test_colour_style_rejects_mono_target(self) -> None:
        with self.assertRaises(DESIGN.DesignProfileError) as raised:
            DESIGN.resolve(_profile("paper_light"),
                           _target("panel_128x64_mono_keypad"))
        self.assertEqual(raised.exception.code, "COLOR_MODE_UNSUPPORTED")

    def test_mono_style_resolves_on_mono_target(self) -> None:
        context = DESIGN.resolve(_profile("mono_industrial"),
                                 _target("panel_128x64_mono_keypad"))
        self.assertEqual(context["resolved"]["motion_level"], "none")
        self.assertEqual(context["resolved"]["max_motion_ms"], 0)
        self.assertEqual(context["resolved"]["max_animated_objects"], 0)
        self.assertEqual(context["resolved"]["min_touch_px"], 0)

    def test_select_falls_back_to_neutral_and_generic(self) -> None:
        context = DESIGN.select_profile(PROFILES_DIR, TARGETS_DIR)
        self.assertEqual(context["profile"]["id"], "neutral")
        self.assertEqual(context["target"]["id"], "generic_constrained")
        self.assertEqual(context["selection"]["profile_source"], "fallback")
        self.assertEqual(context["selection"]["target_source"], "fallback")

    def test_select_uses_active_pair_in_current_mode(self) -> None:
        context = DESIGN.select_profile(
            PROFILES_DIR, TARGETS_DIR,
            active_path=ROOT / "doc/design/active.json",
            mode="current")
        self.assertEqual(context["profile"]["id"], "dark_sparse")
        self.assertEqual(context["target"]["id"], "panel_368x448_touch_60hz")
        self.assertEqual(context["selection"]["mode"], "current")
        self.assertEqual(context["selection"]["profile_source"], "active")
        self.assertEqual(context["selection"]["target_source"], "active")

    def test_select_design_mode_ignores_active_pair(self) -> None:
        context = DESIGN.select_profile(
            PROFILES_DIR, TARGETS_DIR,
            active_path=ROOT / "doc/design/active.json")
        self.assertEqual(context["profile"]["id"], "neutral")
        self.assertEqual(context["target"]["id"], "generic_constrained")
        self.assertEqual(context["selection"]["mode"], "design")
        self.assertEqual(context["selection"]["profile_source"], "fallback")
        self.assertEqual(context["selection"]["target_source"], "fallback")

    def test_select_prefers_explicit_pair(self) -> None:
        context = DESIGN.select_profile(
            PROFILES_DIR, TARGETS_DIR, "paper_light",
            "panel_480x480_touch_60hz", active_path=ROOT / "doc/design/active.json")
        self.assertEqual(context["profile"]["id"], "paper_light")
        self.assertEqual(context["target"]["id"], "panel_480x480_touch_60hz")
        self.assertEqual(context["selection"]["profile_source"], "explicit")
        self.assertEqual(context["selection"]["target_source"], "explicit")

    def test_profile_diversity_requires_three_axes(self) -> None:
        result = DESIGN.profile_diff(_profile("paper_light"),
                                     _profile("soft_playful"))
        self.assertGreaterEqual(result["count"], 3)
        self.assertIn("density", result["different_axes"])
        self.assertIn("corner_scale", result["different_axes"])

    def test_current_profile_matches_c_headers(self) -> None:
        findings = DESIGN.check_current(
            PROFILES_DIR / "dark_sparse.json",
            ROOT / "layers/app_manager/app_theme/include/app_theme_colors.h",
            ROOT / "layers/apps/common/include/app_ui_theme.h",
        )
        self.assertEqual(findings, [])

    def test_contrast_failure_is_rejected(self) -> None:
        profile = copy.deepcopy(_profile("neutral"))
        profile["roles"]["ink"] = profile["roles"]["canvas"]
        errors = DESIGN.validate_profile(profile)
        self.assertTrue(any("contrast ink/canvas" in error for error in errors))

    def test_typography_unsupported_is_reported(self) -> None:
        target = copy.deepcopy(_target("panel_240x320_touch_30hz"))
        target["resources"]["available_font_px"] = [12]
        context = DESIGN.resolve(_profile("dark_sparse"), target)
        codes = {finding["code"] for finding in context["findings"]}
        self.assertIn("TYPOGRAPHY_UNSUPPORTED", codes)
        self.assertFalse(context["resolved"]["typography_supported"])

    def test_validate_pairs_covers_all_pairs(self) -> None:
        profiles = DESIGN.load_profiles(PROFILES_DIR)
        targets = DESIGN.load_targets(TARGETS_DIR)
        result = DESIGN.validate_pairs(profiles, targets)
        self.assertEqual(len(result["pairs"]), len(profiles) * len(targets))
        self.assertEqual(result["failures"], [])

    def test_invalid_scale_is_rejected(self) -> None:
        profile = copy.deepcopy(_profile("neutral"))
        profile["scales"]["space"] = [4, 2, 8]
        errors = DESIGN.validate_profile(profile)
        self.assertTrue(any("strictly increasing" in error for error in errors))

    def test_universal_floor_cannot_be_lowered(self) -> None:
        profile = copy.deepcopy(_profile("neutral"))
        for key in ("min_touch_mm", "min_contrast_body", "min_contrast_large",
                    "min_contrast_ui", "min_contrast_faint"):
            profile["rules"][key] = 0
        errors = DESIGN.validate_profile(profile)
        for key in ("min_touch_mm", "min_contrast_body", "min_contrast_large",
                    "min_contrast_ui", "min_contrast_faint"):
            self.assertTrue(any(key in error for error in errors), key)

    def test_target_font_inventory_matches_app_theme(self) -> None:
        header = (ROOT / "layers/app_manager/app_theme/include/app_theme.h").read_text(
            encoding="utf-8")
        match = re.search(r"#define\s+APP_THEME_FONT_SIZES\s*\{([^}]+)\}", header)
        self.assertIsNotNone(match)
        sizes = sorted(int(value.strip()) for value in match.group(1).split(","))
        target = _target("panel_368x448_touch_60hz")
        self.assertEqual(target["resources"]["available_font_px"], sizes)

    def test_paper_light_reports_typography_unsupported_on_current_target(self) -> None:
        context = DESIGN.resolve(
            _profile("paper_light"), _target("panel_368x448_touch_60hz"))
        codes = {finding["code"] for finding in context["findings"]}
        self.assertIn("TYPOGRAPHY_UNSUPPORTED", codes)
        self.assertFalse(context["resolved"]["typography_supported"])

    def test_blur_downgrade_is_reported(self) -> None:
        profile = copy.deepcopy(_profile("neutral"))
        profile["rules"]["allow_blur"] = True
        target = copy.deepcopy(_target("panel_368x448_touch_60hz"))
        target["resources"]["allow_blur"] = False
        context = DESIGN.resolve(profile, target)
        messages = [finding["message"] for finding in context["findings"]]
        self.assertIn("blur disabled by target", messages)
        self.assertFalse(context["resolved"]["allow_blur"])

    def test_mono_style_resolves_on_rgb565_with_warning(self) -> None:
        context = DESIGN.resolve(
            _profile("mono_industrial"), _target("panel_480x480_touch_60hz"))
        codes = {finding["code"] for finding in context["findings"]}
        self.assertIn("TYPOGRAPHY_UNSUPPORTED", codes)

    def test_select_rejects_active_version_mismatch(self) -> None:
        active = json.loads(
            (ROOT / "doc/design/active.json").read_text(encoding="utf-8"))
        active["profile_version"] = 99
        with tempfile.NamedTemporaryFile(
                "w", suffix=".json", delete=False, encoding="utf-8") as handle:
            json.dump(active, handle)
            path = Path(handle.name)
        try:
            with self.assertRaises(DESIGN.DesignProfileError) as raised:
                DESIGN.select_profile(
                    PROFILES_DIR, TARGETS_DIR, active_path=path, mode="current")
            self.assertEqual(raised.exception.code, "VERSION_MISMATCH")
        finally:
            path.unlink(missing_ok=True)


    def test_extra_style_axis_is_rejected(self) -> None:
        profile = copy.deepcopy(_profile("neutral"))
        profile["style_axes"]["elevation"] = "floating"
        errors = DESIGN.validate_profile(profile)
        self.assertTrue(any("unknown axes" in error for error in errors))

    def test_extra_role_and_root_field_are_rejected(self) -> None:
        profile = copy.deepcopy(_profile("neutral"))
        profile["roles"]["brand_glow"] = "#123456"
        profile["made_up"] = True
        errors = DESIGN.validate_profile(profile)
        self.assertTrue(any("unknown roles" in error for error in errors))
        self.assertTrue(any("unknown fields" in error for error in errors))

    def test_duplicate_profile_ids_are_rejected(self) -> None:
        profile = _profile("neutral")
        with tempfile.TemporaryDirectory() as directory:
            Path(directory, "a.json").write_text(json.dumps(profile), encoding="utf-8")
            Path(directory, "b.json").write_text(json.dumps(profile), encoding="utf-8")
            with self.assertRaises(DESIGN.DesignProfileError) as raised:
                DESIGN.load_profiles(directory)
            self.assertEqual(raised.exception.code, "DUPLICATE_PROFILE")

    def test_contrast_pair_floor_is_applied(self) -> None:
        profile = copy.deepcopy(_profile("neutral"))
        profile["rules"]["contrast_pairs"] = [{
            "foreground": "ink_faint",
            "background": "canvas",
            "floor": "faint",
        }]
        self.assertEqual(DESIGN.validate_profile(profile), [])

    def test_unknown_rule_field_is_rejected(self) -> None:
        profile = copy.deepcopy(_profile("neutral"))
        profile["rules"]["min_touch_m"] = 5.0
        errors = DESIGN.validate_profile(profile)
        self.assertTrue(any("rules: unknown fields" in error for error in errors))

    def test_unknown_compatibility_field_is_rejected(self) -> None:
        profile = copy.deepcopy(_profile("neutral"))
        profile["compatibility"] = {
            "colors": {},
            "app_aliases": {},
            "metric_checks": {},
            "surprise": True,
        }
        errors = DESIGN.validate_profile(profile)
        self.assertTrue(any("compatibility: unknown fields" in error for error in errors))

    def test_invalid_contrast_floor_is_reported_without_exception(self) -> None:
        profile = copy.deepcopy(_profile("neutral"))
        profile["rules"]["min_contrast_body"] = "bad"
        errors = DESIGN.validate_profile(profile)
        self.assertTrue(any("rules.min_contrast_body" in error for error in errors))

    def test_target_orientation_and_emissive_are_validated(self) -> None:
        target = copy.deepcopy(_target("panel_368x448_touch_60hz"))
        target["display"]["orientation"] = "landscape"
        target["display"]["emissive"] = "yes"
        errors = DESIGN.validate_target(target)
        self.assertTrue(any("portrait" in error or "orientation" in error for error in errors))
        self.assertTrue(any("emissive" in error for error in errors))

    def test_check_current_rejects_alias_mismatch(self) -> None:
        ui_header = (ROOT / "layers/apps/common/include/app_ui_theme.h").read_text(
            encoding="utf-8")
        broken = ui_header.replace(
            "#define APP_UI_COLOR_SUN         APP_THEME_COLOR_AMBER",
            "#define APP_UI_COLOR_SUN         APP_THEME_COLOR_CORAL",
        )
        with tempfile.NamedTemporaryFile("w", suffix=".h", delete=False,
                                         encoding="utf-8") as handle:
            handle.write(broken)
            path = Path(handle.name)
        try:
            findings = DESIGN.check_current(
                PROFILES_DIR / "dark_sparse.json",
                ROOT / "layers/app_manager/app_theme/include/app_theme_colors.h",
                path,
            )
            self.assertTrue(any(item["code"] == "ALIAS_DRIFT" for item in findings))
        finally:
            path.unlink(missing_ok=True)

    def test_diff_unknown_profile_reports_unknown_profile(self) -> None:
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            result = DESIGN.main(["diff", "missing_profile", "neutral"])
        self.assertEqual(result, 2)
        self.assertIn("UNKNOWN_PROFILE", output.getvalue())


if __name__ == "__main__":
    unittest.main()
