#!/usr/bin/env python3
"""Tests for the LVGL layout audit helper."""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).parents[1] / "scripts" / "audit_lvgl_layout.py"
SPEC = importlib.util.spec_from_file_location("audit_lvgl_layout", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
AUDIT = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = AUDIT
SPEC.loader.exec_module(AUDIT)


def _codes(source: str, fixed_clusters: bool = False) -> set[str]:
    findings = AUDIT._audit_text(
        Path("fixture.c"), source, fixed_clusters
    )
    return {finding.code for finding in findings}


class AuditLvglLayoutTest(unittest.TestCase):
    def test_multiline_passive_button_child_is_not_reported(self) -> None:
        source = """
void build(void)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_t *content = lv_obj_create(button);
    lv_obj_remove_flag(
        content,
        LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}
"""
        self.assertNotIn("BUTTON_CHILD_INPUT", _codes(source))

    def test_unowned_button_child_is_reported(self) -> None:
        source = """
void build(void)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_t *content = lv_obj_create(button);
}
"""
        self.assertIn("BUTTON_CHILD_INPUT", _codes(source))

    def test_event_bubble_resolves_button_child_input(self) -> None:
        source = """
void build(void)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_t *content = lv_obj_create(button);
    lv_obj_add_flag(content, LV_OBJ_FLAG_EVENT_BUBBLE);
}
"""
        self.assertNotIn("BUTTON_CHILD_INPUT", _codes(source))

    def test_clickable_without_owner_is_still_reported(self) -> None:
        source = """
void build(void)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_t *content = lv_obj_create(button);
    lv_obj_add_flag(content, LV_OBJ_FLAG_CLICKABLE);
}
"""
        self.assertIn("BUTTON_CHILD_INPUT", _codes(source))

    def test_draw_callback_does_not_resolve_button_child_input(self) -> None:
        source = """
void build(void)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_t *content = lv_obj_create(button);
    lv_obj_add_event_cb(content, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
}
"""
        self.assertIn("BUTTON_CHILD_INPUT", _codes(source))

    def test_clicked_callback_resolves_button_child_input(self) -> None:
        source = """
void build(void)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_t *content = lv_obj_create(button);
    lv_obj_add_event_cb(content, clicked_cb, LV_EVENT_CLICKED, NULL);
}
"""
        self.assertNotIn("BUTTON_CHILD_INPUT", _codes(source))

    def test_button_names_do_not_leak_across_functions(self) -> None:
        source = """
void first(void)
{
    lv_obj_t *button = lv_button_create(parent);
}

void second(lv_obj_t *button)
{
    lv_obj_t *content = lv_obj_create(button);
}
"""
        self.assertNotIn("BUTTON_CHILD_INPUT", _codes(source))

    def test_widget_reassignment_updates_object_kind(self) -> None:
        source = """
void build(void)
{
    lv_obj_t *item = lv_label_create(parent);
    lv_obj_set_style_text_font(
        item, app_ui_font(APP_THEME_FONT_BODY), 0);
    lv_label_set_text(item, "value");
    item = lv_obj_create(parent);
    lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *button = lv_button_create(parent);
    button = lv_obj_create(parent);
    lv_obj_t *content = lv_obj_create(button);
}
"""
        codes = _codes(source)
        self.assertNotIn("CLICKABLE_LABEL", codes)
        self.assertNotIn("BUTTON_CHILD_INPUT", codes)

    def test_nested_declaration_shadows_outer_button(self) -> None:
        source = """
void build(void)
{
    lv_obj_t *button = lv_button_create(parent);
    {
        lv_obj_t *button = lv_obj_create(parent);
        lv_obj_t *content = lv_obj_create(button);
    }
}
"""
        self.assertNotIn("BUTTON_CHILD_INPUT", _codes(source))

    def test_late_font_is_reported_without_line_window(self) -> None:
        filler = "\n".join(f"    int value_{index} = {index};"
                           for index in range(20))
        source = f"""
void build(void)
{{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, "value");
{filler}
    lv_obj_set_style_text_font(
        label, app_ui_font(APP_THEME_FONT_BODY), 0);
}}
"""
        self.assertIn("TEXT_BEFORE_SETUP", _codes(source))

    def test_multiline_theme_font_before_text_is_accepted(self) -> None:
        source = """
void build(void)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(
        label,
        app_ui_font(APP_THEME_FONT_BODY),
        0);
    lv_label_set_text(
        label,
        "value");
}
"""
        codes = _codes(source)
        self.assertNotIn("MISSING_EXPLICIT_FONT", codes)
        self.assertNotIn("TEXT_BEFORE_SETUP", codes)
        self.assertNotIn("FONT_ROLE_REVIEW", codes)

    def test_ordinary_text_rejects_default_font(self) -> None:
        source = """
void build(void)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, LV_FONT_DEFAULT, 0);
    lv_label_set_text(label, "value");
}
"""
        self.assertIn("TEXT_FONT_ROLE", _codes(source))

    def test_symbol_requires_default_font(self) -> None:
        source = """
void build(void)
{
    lv_obj_t *icon = lv_label_create(parent);
    lv_obj_set_style_text_font(
        icon, app_ui_font(APP_THEME_FONT_BODY), 0);
    lv_label_set_text(icon, LV_SYMBOL_RIGHT);
}
"""
        self.assertIn("SYMBOL_FONT", _codes(source))

    def test_indirect_symbol_requires_font_role_review(self) -> None:
        source = """
void build(const char *symbol)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, LV_FONT_DEFAULT, 0);
    lv_label_set_text(label, symbol);
}
"""
        codes = _codes(source)
        self.assertNotIn("SYMBOL_FONT", codes)
        self.assertNotIn("TEXT_FONT_ROLE", codes)
        self.assertIn("FONT_ROLE_REVIEW", codes)

    def test_symbol_helper_requires_font_role_review(self) -> None:
        source = """
void build(void)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, LV_FONT_DEFAULT, 0);
    lv_label_set_text(label, lookup_symbol());
}
"""
        codes = _codes(source)
        self.assertIn("FONT_ROLE_REVIEW", codes)
        self.assertNotIn("TEXT_FONT_ROLE", codes)

    def test_ordinary_text_on_icon_named_label_uses_theme_font(self) -> None:
        source = """
void build(void)
{
    lv_obj_t *icon = lv_label_create(parent);
    lv_obj_set_style_text_font(
        icon, app_ui_font(APP_THEME_FONT_BODY), 0);
    lv_label_set_text(icon, "Network name");
}
"""
        codes = _codes(source)
        self.assertNotIn("SYMBOL_FONT", codes)
        self.assertNotIn("FONT_ROLE_REVIEW", codes)

    def test_comments_do_not_create_findings(self) -> None:
        source = """
void build(void)
{
    /* lv_obj_set_pos(fake, 1, 2);
       lv_label_set_long_mode(fake, LV_LABEL_LONG_DOT); */
}
"""
        self.assertEqual(set(), _codes(source))

    def test_preprocessor_aliases_do_not_create_findings(self) -> None:
        source = """
#define LV_LABEL_LONG_DOT LV_LABEL_LONG_MODE_DOTS
#define POSITION_OBJECT(obj) \\
    lv_obj_set_pos((obj), 1, 2)
void build(void)
{
}
"""
        self.assertEqual(set(), _codes(source))

    def test_absolute_dot_and_fixed_cluster_are_reported(self) -> None:
        source = """
void build(void)
{
    lv_obj_set_pos(one, 1, 2);
    lv_obj_set_size(two, 3, 4);
    lv_obj_set_width(three, 5);
    lv_obj_set_height(four, 6);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
}
"""
        codes = _codes(source, fixed_clusters=True)
        self.assertIn("ABSOLUTE_POSITION", codes)
        self.assertIn("DOT_TRUNCATION", codes)
        self.assertIn("FIXED_GEOMETRY_CLUSTER", codes)

    def test_fixed_geometry_clusters_do_not_cross_functions(self) -> None:
        source = """
void first(void)
{
    lv_obj_set_width(one, 1);
    lv_obj_set_height(one, 2);
}
void second(void)
{
    lv_obj_set_width(two, 3);
    lv_obj_set_height(two, 4);
}
"""
        self.assertNotIn(
            "FIXED_GEOMETRY_CLUSTER", _codes(source, fixed_clusters=True)
        )

    def test_empty_source_directory_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run(
                [sys.executable, str(SCRIPT), "--strict", directory],
                check=False,
                capture_output=True,
                text=True,
            )
        self.assertEqual(2, result.returncode)
        self.assertIn("no C/C++ source files", result.stderr)

    def test_strict_findings_return_one(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.c"
            source.write_text(
                "void build(void) { lv_obj_set_pos(obj, 1, 2); }\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [sys.executable, str(SCRIPT), "--strict", str(source)],
                check=False,
                capture_output=True,
                text=True,
            )
        self.assertEqual(1, result.returncode)
        self.assertIn("ABSOLUTE_POSITION", result.stdout)

    def test_decode_failure_returns_two(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.c"
            source.write_bytes(b"\xff\xfe\x00")
            result = subprocess.run(
                [sys.executable, str(SCRIPT), str(source)],
                check=False,
                capture_output=True,
                text=True,
            )
        self.assertEqual(2, result.returncode)
        self.assertIn("cannot decode", result.stderr)


if __name__ == "__main__":
    unittest.main()
