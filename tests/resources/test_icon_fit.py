"""Validate that app icons survive the 1:1 (clipping) draw windows.

LVGL draws a plain `lv_image` at its natural size inside the object box and
clips the overflow, so a record exported larger than the object a consumer
creates is silently cropped. The launcher draws app icons in its 72 px well and
the task switcher in a 56 px window; the home cards draw their glyphs at 40 px.

The window sizes are read from the consumers' own constants, not restated here:
changing a consumer's size re-derives this table and fails the icons that no
longer fit, instead of leaving a stale number behind.
"""

from pathlib import Path
import re
import unittest

from PIL import Image


PROJECT_ROOT = Path(__file__).resolve().parents[2]
APPS_DIR = PROJECT_ROOT / "layers" / "apps"
SHARED_APP_SOURCES = APPS_DIR / "app_sources.cmake"
APP_CORE_SRC = PROJECT_ROOT / "layers" / "app_manager" / "app_core" / "src"

# Which draw sites each icon record appears in. Sizes come from the code below.
#   launcher_tile - `app_ui_add_icon_tile` image inside the launcher well
#   switcher      - task-switcher card icon
#   home_card     - home complication glyph (`_home_card_glyph`)
#   ring_glyph    - home clock card glyph inside the progress ring
APP_ICON = ("launcher_tile", "switcher")
HOME_ICON_ROLES = {
    "APP_IMAGE_HOME_ICON": APP_ICON,
    "APP_IMAGE_MENU_ICON": APP_ICON,
    "APP_IMAGE_SETTINGS_ICON": APP_ICON,
    "APP_IMAGE_SETUP_ICON": APP_ICON,
    "APP_IMAGE_CLOCK_ICON": APP_ICON,
    "APP_IMAGE_RECORDER_ICON": APP_ICON,
    "APP_IMAGE_LEVEL_ICON": APP_ICON,
    "APP_IMAGE_CALENDAR_ICON": APP_ICON,
    "APP_IMAGE_CALCULATOR_ICON": APP_ICON,
    "APP_IMAGE_DIAGNOSTICS_ICON": APP_ICON,
    "APP_IMAGE_WEATHER_APP": APP_ICON,
    # Complication glyphs appear only on their home card.
    "APP_IMAGE_HOME_CALCULATOR": ("home_card",),
    "APP_IMAGE_HOME_CALENDAR": ("home_card",),
    # The 28 px ring glyph is the one record authored for a single small use.
    "APP_IMAGE_HOME_RING_CLOCK": ("ring_glyph",),
}

# The renderer composites the SVG onto the opaque black canvas, so only ink
# that survives the alpha channel and clears the panel floor counts.
ALPHA_FLOOR = 40
INK_FLOOR = 40
CLEARANCE = 1


def _launcher_tile_image_px() -> int:
    """Image size `app_ui_add_icon_tile` gives its picture."""
    text = (APPS_DIR / "common" / "src" / "app_ui.c").read_text(
        encoding="utf-8")
    start = text.index("lv_obj_t *app_ui_add_icon_tile(")
    match = re.search(r"lv_obj_set_size\(image, (\d+), (\d+)\)",
                      text[start:])
    assert match, "app_ui_add_icon_tile no longer sizes its image"
    return int(match.group(1))


def _switcher_icon_px() -> int:
    text = (APP_CORE_SRC / "app_manager_task_switcher.c").read_text(
        encoding="utf-8")
    match = re.search(r"#define TS_ICON_SIZE\s+(\d+)", text)
    assert match, "task switcher no longer defines TS_ICON_SIZE"
    return int(match.group(1))


def _home_card_px() -> int:
    """Glyph size `_home_card_glyph` passes to `app_ui_image`."""
    text = (APPS_DIR / "home_app" / "src" / "home_app.c").read_text(
        encoding="utf-8")
    start = text.index("static void _home_card_glyph(")
    match = re.search(r"app_ui_image\(row, image_id, fallback_symbol, (\d+)\)",
                      text[start:])
    assert match, "_home_card_glyph no longer sizes its glyph"
    return int(match.group(1))


def _ring_glyph_px() -> int:
    text = (APPS_DIR / "home_app" / "src" / "home_app.c").read_text(
        encoding="utf-8")
    start = text.index("_home_ring_glyph")
    match = re.search(r"lv_obj_set_size\(image, (\d+), (\d+)\)", text[start:])
    assert match, "_home_ring_glyph no longer sizes its image"
    return int(match.group(1))


def _draw_windows() -> dict:
    sizes = {
        "launcher_tile": _launcher_tile_image_px(),
        "switcher": _switcher_icon_px(),
        "home_card": _home_card_px(),
        "ring_glyph": _ring_glyph_px(),
    }
    return {token: tuple(sizes[role] for role in roles)
            for token, roles in HOME_ICON_ROLES.items()}


def _records() -> list:
    records = []
    for manifest in sorted(APPS_DIR.glob("*/resource_manifest.cmake")):
        text = manifest.read_text(encoding="utf-8")
        for source, _output, semantic, width, height, kind in re.findall(
            r'"([^|"]+)\|([^|"]+)\|([^|"]+)\|(\d+)\|(\d+)\|(\w+)"', text
        ):
            if kind != "SVG" or semantic not in HOME_ICON_ROLES:
                continue
            path = (manifest.parent / source.replace(
                "${CMAKE_CURRENT_LIST_DIR}/", "")).resolve()
            records.append((semantic, path, int(width), int(height)))
    return records


def _rasterize(path: Path, width: int, height: int) -> Image.Image:
    """Rasterize one SVG exactly the way the build pipeline does."""
    import os
    import subprocess
    import sys
    import tempfile

    if "currentColor" in path.read_text(encoding="utf-8"):
        raise AssertionError(f"{path} must bake explicit colors")
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "icon.png")
        subprocess.run(
            [sys.executable,
             str(PROJECT_ROOT / "tools" / "asset_pipeline" / "svg2png.py"),
             str(path), "--width", str(width), "--height", str(height),
             "--output", out],
            check=True, capture_output=True)
        return Image.open(out).convert("RGBA")


def _ink_bbox(image: Image.Image):
    """Bounding box of alpha-weighted lit pixels, or None for a blank asset."""
    pixels = image.convert("RGBA").load()
    width, height = image.size
    xs = []
    ys = []
    for y in range(height):
        for x in range(width):
            r, g, b, a = pixels[x, y]
            if a > ALPHA_FLOOR and (r + g + b) / 3 * (a / 255) > INK_FLOOR:
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    return min(xs), max(xs), min(ys), max(ys)


class IconFitTest(unittest.TestCase):
    """An icon must stay inside every window a consumer draws it into."""

    def test_every_icon_record_declares_its_draw_sites(self) -> None:
        for semantic, path, _width, _height in _records():
            with self.subTest(icon=semantic):
                self.assertIn(semantic, HOME_ICON_ROLES,
                              f"{semantic} has no draw site recorded")
                self.assertTrue(path.is_file(), path)

    def test_icon_ink_fits_every_draw_window(self) -> None:
        records = _records()
        self.assertGreater(len(records), 10)
        windows = _draw_windows()
        for semantic, path, width, height in records:
            with self.subTest(icon=semantic):
                image = _rasterize(path, width, height)
                bbox = _ink_bbox(image)
                self.assertIsNotNone(bbox, f"{path} renders nothing")
                for window in windows[semantic]:
                    low = (width - window) // 2 + CLEARANCE
                    high = width - 1 - (width - window) // 2 - CLEARANCE
                    self.assertGreaterEqual(bbox[0], low,
                                            f"{semantic} clipped left in {window}")
                    self.assertLessEqual(bbox[1], high,
                                         f"{semantic} clipped right in {window}")
                    self.assertGreaterEqual(bbox[2], low,
                                            f"{semantic} clipped top in {window}")
                    self.assertLessEqual(bbox[3], high,
                                         f"{semantic} clipped bottom in {window}")


if __name__ == "__main__":
    unittest.main()