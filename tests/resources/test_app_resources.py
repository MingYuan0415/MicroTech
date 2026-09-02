"""Validate app-owned resource layout and the SVG-first image sources."""

from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET


PROJECT_ROOT = Path(__file__).resolve().parents[2]
APPS_DIR = PROJECT_ROOT / "layers" / "apps"
APP_DIRS = (
    "home_app",
    "menu_app",
    "settings_app",
    "setup_app",
    "clock_app",
    "recorder_app",
    "level_app",
    "diagnostics_app",
)


def _assert_final_colored_svg(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    assert "currentColor" not in text, f"{path} must bake explicit colors"
    root = ET.fromstring(text[text.index("<svg"):])
    assert root.get("viewBox"), f"{path} needs a viewBox"


class AppResourceTest(unittest.TestCase):
    def test_app_icons_are_svg_sources(self) -> None:
        for app in APP_DIRS:
            with self.subTest(app=app):
                icon = APPS_DIR / app / "assets" / "icon.svg"
                self.assertTrue(icon.is_file(), icon)
                _assert_final_colored_svg(icon)

    def test_repository_has_no_bitmap_sources(self) -> None:
        leftovers = sorted(
            str(path.relative_to(APPS_DIR))
            for path in APPS_DIR.rglob("*.png")
            if "build" not in path.parts
        )
        self.assertEqual([], leftovers)

    def test_theme_font_exists(self) -> None:
        font = PROJECT_ROOT / "layers" / "app_manager" / "app_theme" / "assets" / "font.ttf"
        self.assertTrue(font.is_file())
        self.assertGreater(font.stat().st_size, 0)

    def test_manifests_are_explicit_and_sources_exist(self) -> None:
        manifests = [
            APPS_DIR / "resource_manifest.cmake",
            PROJECT_ROOT / "layers" / "app_manager" / "app_theme" / "resource_manifest.cmake",
        ]
        for manifest in manifests:
            self.assertTrue(manifest.is_file())
            text = manifest.read_text()
            self.assertNotIn("GLOB", text)
        records = 0
        record_manifests = sorted(
            APPS_DIR.glob("*/resource_manifest.cmake")
        ) + [
            PROJECT_ROOT
            / "layers"
            / "app_manager"
            / "app_theme"
            / "resource_manifest.cmake"
        ]
        for record_manifest in record_manifests:
            for source, output, semantic, width, height, kind in re.findall(
                r'"([^|"]+)\|([^|"]+)\|([^|"]+)\|(\d+)\|(\d+)\|(\w+)"',
                record_manifest.read_text(),
            ):
                records += 1
                source_path = (record_manifest.parent / source.replace(
                    "${CMAKE_CURRENT_LIST_DIR}/", ""
                )).resolve()
                self.assertTrue(source_path.is_file(), source_path)
                self.assertIn(kind, {"SVG", "PNG", "FONT"})
                if kind in {"SVG", "PNG"}:
                    self.assertGreater(int(width), 0)
                    self.assertGreater(int(height), 0)
                self.assertTrue(output)
                self.assertNotIn(output, getattr(self, "_outputs", set()))
                self.assertNotIn(semantic, getattr(self, "_semantics", set()))
                self._outputs = getattr(self, "_outputs", set()) | {output}
                if semantic != "0":
                    self._semantics = getattr(self, "_semantics", set()) | {semantic}
        self.assertGreater(records, 40)


if __name__ == "__main__":
    unittest.main()
