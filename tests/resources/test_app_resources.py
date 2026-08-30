"""Validate app-owned resource layout and image formats."""

from pathlib import Path
import re
import struct
import unittest


PROJECT_ROOT = Path(__file__).resolve().parents[2]
APP_DIRS = ("home_app", "menu_app", "settings_app", "setup_app")


def _png_header(path: Path) -> tuple[int, int, int, int]:
    with path.open("rb") as image:
        if image.read(8) != b"\x89PNG\r\n\x1a\n":
            raise AssertionError(f"{path} is not a PNG")
        length = struct.unpack(">I", image.read(4))[0]
        if image.read(4) != b"IHDR":
            raise AssertionError(f"{path} has no IHDR")
        payload = image.read(length)
    return struct.unpack(">IIBB", payload[:10])


class AppResourceTest(unittest.TestCase):
    def test_app_icons_are_64px_rgba(self) -> None:
        for app in APP_DIRS:
            with self.subTest(app=app):
                header = _png_header(
                    PROJECT_ROOT / "layers" / "apps" / app / "assets" / "icon.png"
                )
                self.assertEqual((64, 64, 8, 6), header)

    def test_theme_font_exists(self) -> None:
        font = PROJECT_ROOT / "layers" / "app_manager" / "app_theme" / "assets" / "font.ttf"
        self.assertTrue(font.is_file())
        self.assertGreater(font.stat().st_size, 0)

    def test_manifests_are_explicit_and_sources_exist(self) -> None:
        manifests = [
            PROJECT_ROOT / "layers" / "apps" / "resource_manifest.cmake",
            PROJECT_ROOT / "layers" / "app_manager" / "app_theme" / "resource_manifest.cmake",
        ]
        for manifest in manifests:
            self.assertTrue(manifest.is_file())
            text = manifest.read_text()
            self.assertNotIn("GLOB", text)
            for source, output, semantic in re.findall(
                r'"([^|]+)\|([^|]+)\|([^|]+)\|', text
            ):
                source_path = (manifest.parent / source.replace(
                    "${CMAKE_CURRENT_LIST_DIR}/", ""
                )).resolve()
                self.assertTrue(source_path.is_file(), source_path)
                self.assertTrue(output)
                self.assertNotIn(output, getattr(self, "_outputs", set()))
                self.assertNotIn(semantic, getattr(self, "_semantics", set()))
                self._outputs = getattr(self, "_outputs", set()) | {output}
                if semantic != "0":
                    self._semantics = getattr(self, "_semantics", set()) | {semantic}


if __name__ == "__main__":
    unittest.main()
