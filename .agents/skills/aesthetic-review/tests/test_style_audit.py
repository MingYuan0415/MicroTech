#!/usr/bin/env python3
"""Tests for the profile-aware LVGL tree audit."""

from __future__ import annotations

import copy
import importlib.util
import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).parents[4]
PROFILE_SCRIPT = ROOT / ".agents" / "skills" / "design-contract" / "scripts" / "design_profile.py"
AUDIT_SCRIPT = Path(__file__).parents[1] / "scripts" / "style_audit.py"


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


DESIGN = _load("design_profile", PROFILE_SCRIPT)
AUDIT = _load("style_audit", AUDIT_SCRIPT)
PROFILES = ROOT / "doc" / "design" / "profiles"
TARGETS = ROOT / "doc" / "design" / "targets"


def _context(profile: str = "dark_sparse", target: str = "panel_368x448_touch_60hz") -> dict:
    profile_data = json.loads((PROFILES / f"{profile}.json").read_text(encoding="utf-8"))
    target_data = json.loads((TARGETS / f"{target}.json").read_text(encoding="utf-8"))
    return DESIGN.resolve(profile_data, target_data)


def _node(kind: str = "lv_obj", x: int = 0, y: int = 0, w: int = 100, h: int = 100,
          **styles) -> dict:
    return {
        "type": kind,
        "coords": {"x": x, "y": y, "w": w, "h": h},
        "styles": styles,
        "children": [],
    }


def _codes(result: dict) -> set[str]:
    return {finding["code"] for finding in result["findings"]}


class StyleAuditTest(unittest.TestCase):
    def test_clean_tree_passes(self) -> None:
        context = _context()
        display = context["target"]["display"]
        root = _node("lv_obj", w=display["width"], h=display["height"],
                     bg_color=context["resolved"]["colors"]["canvas"], bg_opa=255)
        label = _node("lv_label", x=16, y=16, w=200, h=30,
                      text="Settings", text_color=context["resolved"]["colors"]["ink"], bg_opa=0)
        root["children"].append(label)
        result = AUDIT.audit_tree(root, context)
        self.assertEqual(result["findings"], [])

    def test_colour_outside_profile_is_reported(self) -> None:
        context = _context()
        root = _node("lv_obj", w=200, h=100, bg_color="#123456", bg_opa=255)
        result = AUDIT.audit_tree(root, context)
        self.assertIn("BG_COLOR_NOT_IN_PROFILE", _codes(result))

    def test_low_contrast_is_a_blocker(self) -> None:
        context = copy.deepcopy(_context())
        context["resolved"]["colors"]["ink"] = "#777777"
        root = _node("lv_obj", w=200, h=100, bg_color="#FFFFFF", bg_opa=255)
        label = _node("lv_label", w=100, h=30, text="Low", text_color="#777777", bg_opa=0)
        root["children"].append(label)
        result = AUDIT.audit_tree(root, context)
        self.assertIn("CONTRAST_FAIL", _codes(result))

    def test_touch_target_below_profile_minimum(self) -> None:
        context = _context()
        root = _node("lv_obj", w=368, h=448, bg_color="#000000", bg_opa=255)
        button = _node("lv_button", w=40, h=40, bg_color="#000000", bg_opa=255)
        button["flags"] = {"clickable": True}
        root["children"].append(button)
        result = AUDIT.audit_tree(root, context)
        self.assertIn("TOUCH_TARGET_SMALL", _codes(result))

    def test_off_scale_radius_and_spacing(self) -> None:
        context = _context()
        root = _node("lv_obj", w=368, h=448, bg_color="#000000", bg_opa=255)
        card = _node("lv_obj", x=16, y=16, w=300, h=100,
                     bg_color=context["resolved"]["colors"]["surface_1"], bg_opa=255, radius=13,
                     pad_left=13, pad_right=13, pad_top=16, pad_bottom=16)
        root["children"].append(card)
        result = AUDIT.audit_tree(root, context)
        self.assertIn("OFF_SCALE_RADIUS", _codes(result))
        self.assertIn("OFF_SCALE_SPACING", _codes(result))

    def test_profile_mismatch_is_reported(self) -> None:
        context = _context("dark_sparse")
        tree = {"profile_id": "paper_light", "tree": _node("lv_obj")}
        result = AUDIT.audit_tree(tree, context)
        self.assertIn("PROFILE_MISMATCH", _codes(result))

    def test_overlay_roots_are_audited(self) -> None:
        context = _context()
        root = _node("lv_obj", w=368, h=448,
                     bg_color=context["resolved"]["colors"]["canvas"], bg_opa=255)
        overlay = _node("lv_obj", w=368, h=448,
                        bg_color="#123456", bg_opa=255)
        result = AUDIT.audit_tree({"tree": root, "overlays": [overlay]}, context)
        self.assertIn("BG_COLOR_NOT_IN_PROFILE", _codes(result))

    def test_clickable_row_height_is_within_touch_target(self) -> None:
        context = _context()
        root = _node("lv_obj", w=368, h=448, bg_color="#000000", bg_opa=255)
        row = _node("lv_button", x=20, y=76, w=328, h=60,
                    bg_color=context["resolved"]["colors"]["surface_1"], bg_opa=255)
        row["flags"] = {"clickable": True}
        root["children"].append(row)
        result = AUDIT.audit_tree(root, context)
        self.assertNotIn("TOUCH_TARGET_SMALL", _codes(result))

    def test_track_and_tint_roles_are_part_of_the_palette(self) -> None:
        context = _context()
        for role in ("track", "accent_tint", "success_tint", "warning_tint", "danger_tint"):
            colour = context["resolved"]["colors"][role]
            node = _node("lv_obj", w=100, h=100, bg_color=colour, bg_opa=255)
            result = AUDIT.audit_tree(node, context)
            self.assertNotIn("BG_COLOR_NOT_IN_PROFILE", _codes(result), colour)

    def test_accent_role_budget_is_checked(self) -> None:
        context = _context()
        root = _node("lv_obj", w=368, h=448, bg_color="#000000", bg_opa=255)
        colors = context["resolved"]["colors"]
        for index, role in enumerate(("accent", "success", "warning")):
            root["children"].append(
                _node("lv_obj", x=index * 20, y=0, w=10, h=10,
                      bg_color=colors[role], bg_opa=255))
        result = AUDIT.audit_tree(root, context)
        self.assertIn("TOO_MANY_ACCENT_ROLES", _codes(result))

    def test_icon_budget_is_checked(self) -> None:
        context = copy.deepcopy(_context())
        context["resolved"]["max_icon_count"] = 1
        root = _node("lv_obj", w=368, h=448, bg_color="#000000", bg_opa=255)
        for index, image_id in enumerate(("ui_one", "ui_two")):
            image = _node("lv_image", x=index * 32, y=0, w=28, h=28, bg_opa=0)
            image["image_semantic_id"] = image_id
            root["children"].append(image)
        result = AUDIT.audit_tree(root, context)
        self.assertIn("TOO_MANY_ICONS", _codes(result))

    def test_opaque_root_cover_is_not_reported_as_background_colour(self) -> None:
        context = _context()
        root = _node("lv_obj", w=368, h=448, bg_color="#F5F5F5", bg_opa=255)
        cover = _node("lv_obj", w=368, h=448, bg_color="#000000", bg_opa=255)
        root["children"].append(cover)
        result = AUDIT.audit_tree(root, context)
        self.assertEqual(result["findings"], [])

    def test_mono_context_accepts_monochrome_tree(self) -> None:
        context = _context("mono_industrial", "panel_128x64_mono_keypad")
        root = _node("lv_obj", w=128, h=64,
                     bg_color=context["resolved"]["colors"]["canvas"], bg_opa=255)
        label = _node("lv_label", w=80, h=12, text="MENU",
                      text_color=context["resolved"]["colors"]["ink"], bg_opa=0)
        root["children"].append(label)
        result = AUDIT.audit_tree(root, context)
        self.assertEqual(result["findings"], [])

    def test_hidden_subtree_is_ignored(self) -> None:
        context = _context()
        root = _node("lv_obj", w=368, h=448,
                     bg_color=context["resolved"]["colors"]["canvas"], bg_opa=255)
        hidden = _node("lv_obj", w=100, h=100, bg_color="#123456", bg_opa=255)
        hidden["flags"] = {"hidden": True}
        root["children"].append(hidden)
        result = AUDIT.audit_tree(root, context)
        self.assertNotIn("BG_COLOR_NOT_IN_PROFILE", _codes(result))

    def test_hidden_cover_does_not_hide_parent_background(self) -> None:
        context = _context()
        root = _node("lv_obj", w=368, h=448, bg_color="#F5F5F5", bg_opa=255)
        hidden = _node("lv_obj", w=368, h=448,
                       bg_color=context["resolved"]["colors"]["canvas"], bg_opa=255)
        hidden["flags"] = {"hidden": True}
        root["children"].append(hidden)
        result = AUDIT.audit_tree(root, context)
        self.assertIn("BG_COLOR_NOT_IN_PROFILE", _codes(result))

    def test_ext_click_pad_expands_touch_target(self) -> None:
        context = _context()
        root = _node("lv_obj", w=368, h=448,
                     bg_color=context["resolved"]["colors"]["canvas"], bg_opa=255)
        slider = _node("lv_slider", w=312, h=16, ext_click_pad=22)
        slider["flags"] = {"clickable": True}
        root["children"].append(slider)
        result = AUDIT.audit_tree(root, context)
        self.assertNotIn("TOUCH_TARGET_SMALL", _codes(result))

    def test_clickable_parent_is_used_for_nested_switch(self) -> None:
        context = _context()
        root = _node("lv_obj", w=368, h=448,
                     bg_color=context["resolved"]["colors"]["canvas"], bg_opa=255)
        row = _node("lv_button", w=328, h=72)
        row["flags"] = {"clickable": True}
        switch = _node("lv_switch", w=60, h=34)
        switch["flags"] = {"clickable": True}
        row["children"].append(switch)
        root["children"].append(row)
        result = AUDIT.audit_tree(root, context)
        self.assertNotIn("TOUCH_TARGET_SMALL", _codes(result))

    def test_duplicate_accent_colour_is_counted(self) -> None:
        context = _context("paper_light", "panel_240x320_touch_30hz")
        root = _node("lv_obj", w=240, h=320,
                     bg_color=context["resolved"]["colors"]["canvas"], bg_opa=255)
        colors = context["resolved"]["colors"]
        for role in ("accent", "success", "warning"):
            root["children"].append(_node("lv_obj", w=10, h=10,
                                          bg_color=colors[role], bg_opa=255))
        result = AUDIT.audit_tree(root, context)
        self.assertIn("TOO_MANY_ACCENT_ROLES", _codes(result))

    def test_mono_target_does_not_report_accent_roles(self) -> None:
        context = _context("mono_industrial", "panel_128x64_mono_keypad")
        root = _node("lv_obj", w=128, h=64,
                     bg_color=context["resolved"]["colors"]["canvas"], bg_opa=255)
        button = _node("lv_button", w=40, h=20, bg_color="#000000", bg_opa=255)
        button["flags"] = {"clickable": True}
        root["children"].append(button)
        result = AUDIT.audit_tree(root, context)
        self.assertNotIn("TOO_MANY_ACCENT_ROLES", _codes(result))


    def test_visible_false_subtree_is_ignored(self) -> None:
        context = _context()
        root = _node("lv_obj", w=368, h=448,
                     bg_color=context["resolved"]["colors"]["canvas"], bg_opa=255)
        hidden = _node("lv_obj", w=100, h=100, bg_color="#123456", bg_opa=255)
        hidden["flags"] = {"visible": False}
        root["children"].append(hidden)
        result = AUDIT.audit_tree(root, context)
        self.assertNotIn("BG_COLOR_NOT_IN_PROFILE", _codes(result))

    def test_passive_button_is_not_a_touch_target(self) -> None:
        context = _context()
        root = _node("lv_obj", w=368, h=448,
                     bg_color=context["resolved"]["colors"]["canvas"], bg_opa=255)
        button = _node("lv_button", w=30, h=30, bg_color=context["resolved"]["colors"]["canvas"], bg_opa=255)
        button["flags"] = {"clickable": False}
        root["children"].append(button)
        result = AUDIT.audit_tree(root, context)
        self.assertNotIn("TOUCH_TARGET_SMALL", _codes(result))

    def test_transparent_radius_is_not_checked(self) -> None:
        context = _context()
        root = _node("lv_obj", w=368, h=448,
                     bg_color=context["resolved"]["colors"]["canvas"], bg_opa=255)
        button = _node("lv_button", w=200, h=40, radius=10, bg_opa=0,
                       border_width=0)
        button["flags"] = {"clickable": True}
        root["children"].append(button)
        result = AUDIT.audit_tree(root, context)
        self.assertNotIn("OFF_SCALE_RADIUS", _codes(result))

    def test_visible_border_colour_not_in_profile_is_reported(self) -> None:
        context = _context()
        root = _node("lv_obj", w=368, h=448,
                     bg_color=context["resolved"]["colors"]["canvas"], bg_opa=255)
        card = _node("lv_obj", w=200, h=100, bg_opa=0, radius=12,
                     border_color="#123456", border_width=2, border_opa=255)
        root["children"].append(card)
        result = AUDIT.audit_tree(root, context)
        self.assertIn("BORDER_COLOR_NOT_IN_PROFILE", _codes(result))

    def test_transparent_border_does_not_enable_radius(self) -> None:
        context = _context()
        root = _node("lv_obj", w=368, h=448,
                     bg_color=context["resolved"]["colors"]["canvas"], bg_opa=255)
        card = _node("lv_obj", w=200, h=100, bg_opa=0, radius=13,
                     border_width=1, border_opa=0)
        root["children"].append(card)
        result = AUDIT.audit_tree(root, context)
        self.assertNotIn("OFF_SCALE_RADIUS", _codes(result))

    def test_clickable_lv_obj_is_a_touch_target(self) -> None:
        context = _context()
        root = _node("lv_obj", w=368, h=448,
                     bg_color=context["resolved"]["colors"]["canvas"], bg_opa=255)
        tile = _node("lv_obj", w=30, h=30,
                     bg_color=context["resolved"]["colors"]["canvas"], bg_opa=255)
        tile["flags"] = {"clickable": True}
        root["children"].append(tile)
        result = AUDIT.audit_tree(root, context)
        self.assertIn("TOUCH_TARGET_SMALL", _codes(result))

    def test_clickable_lv_obj_row_covers_nested_switch(self) -> None:
        context = _context()
        root = _node("lv_obj", w=368, h=448,
                     bg_color=context["resolved"]["colors"]["canvas"], bg_opa=255)
        row = _node("lv_obj", w=328, h=60)
        row["flags"] = {"clickable": True, "scrollable": False}
        switch = _node("lv_switch", w=60, h=34)
        switch["flags"] = {"clickable": True}
        row["children"].append(switch)
        root["children"].append(row)
        result = AUDIT.audit_tree(root, context)
        self.assertNotIn("TOUCH_TARGET_SMALL", _codes(result))

    def test_clickable_full_page_container_does_not_mask_small_child(self) -> None:
        context = _context()
        root = _node("lv_obj", w=368, h=448,
                     bg_color=context["resolved"]["colors"]["canvas"], bg_opa=255)
        scrim = _node("lv_obj", w=368, h=448,
                      bg_color=context["resolved"]["colors"]["canvas"], bg_opa=255)
        scrim["flags"] = {"clickable": True, "scrollable": False}
        button = _node("lv_button", w=40, h=40,
                       bg_color=context["resolved"]["colors"]["canvas"], bg_opa=255)
        button["flags"] = {"clickable": True}
        scrim["children"].append(button)
        root["children"].append(scrim)
        result = AUDIT.audit_tree(root, context)
        self.assertIn("TOUCH_TARGET_SMALL", _codes(result))

    def test_unknown_image_is_reported_and_not_counted_as_known(self) -> None:
        context = copy.deepcopy(_context())
        context["resolved"]["max_icon_count"] = 1
        root = _node("lv_obj", w=368, h=448,
                     bg_color=context["resolved"]["colors"]["canvas"], bg_opa=255)
        for index in range(3):
            image = _node("lv_image", x=index * 40, y=0, w=32, h=32)
            image["image_semantic_id"] = "unknown"
            root["children"].append(image)
        result = AUDIT.audit_tree(root, context)
        self.assertIn("UNKNOWN_IMAGE", _codes(result))
        self.assertNotIn("TOO_MANY_ICONS", _codes(result))

    def test_pad_row_is_checked_when_spacing_enabled(self) -> None:
        context = _context()
        root = _node("lv_obj", w=368, h=448,
                     bg_color=context["resolved"]["colors"]["canvas"], bg_opa=255,
                     pad_row=13)
        result = AUDIT.audit_tree(root, context, check_spacing=True)
        self.assertIn("OFF_SCALE_SPACING", _codes(result))


if __name__ == "__main__":
    unittest.main()
