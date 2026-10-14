"""Validate that app icons survive the 1:1 (clipping) draw windows.

LVGL draws a plain `lv_image` at its natural size inside the object box and
clips the overflow, so a record exported larger than the object a consumer
creates is silently cropped. The launcher and the home dock both draw app icons
in a 40 px window while the manifest exports them at 64 px / 52 px.
"""

from pathlib import Path
import re
import unittest

from PIL import Image


PROJECT_ROOT = Path(__file__).resolve().parents[2]
APPS_DIR = PROJECT_ROOT / "layers" / "apps"

# Every drawn window a record is rendered into, keyed by semantic token.
# Values are the pixel sizes of the LVGL image object; a record larger than its
# window is cropped by the renderer.
WINDOWS = {
    "APP_IMAGE_HOME_ICON": (40, 56),
    "APP_IMAGE_MENU_ICON": (40, 56),
    "APP_IMAGE_SETTINGS_ICON": (40, 56),
    "APP_IMAGE_SETUP_ICON": (40, 56),
    "APP_IMAGE_CLOCK_ICON": (40, 56),
    "APP_IMAGE_RECORDER_ICON": (40, 56),
    "APP_IMAGE_LEVEL_ICON": (40, 56),
    "APP_IMAGE_CALENDAR_ICON": (40, 56),
    "APP_IMAGE_CALCULATOR_ICON": (40, 56),
    "APP_IMAGE_DIAGNOSTICS_ICON": (40, 56),
    "APP_IMAGE_WEATHER_APP": (40, 56),
    "APP_IMAGE_HOME_CLOCK": (40,),
    "APP_IMAGE_HOME_RECORDER": (40,),
    "APP_IMAGE_HOME_LEVEL": (40,),
    "APP_IMAGE_HOME_SETTINGS": (40,),
    "APP_IMAGE_HOME_RING_CLOCK": (28,),
}

# The renderer composites the SVG onto the opaque black canvas, so only ink
# that survives the alpha channel and clears the panel floor counts.
ALPHA_FLOOR = 40
INK_FLOOR = 40
CLEARANCE = 1


def _records() -> list:
    records = []
    for manifest in sorted(APPS_DIR.glob("*/resource_manifest.cmake")):
        text = manifest.read_text(encoding="utf-8")
        for source, output, semantic, width, height, kind in re.findall(
            r'"([^|"]+)\|([^|"]+)\|([^|"]+)\|(\d+)\|(\d+)\|(\w+)"', text
        ):
            if kind != "SVG" or semantic not in WINDOWS:
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

    def test_icon_records_have_a_known_window(self) -> None:
        for manifest in sorted(APPS_DIR.glob("*/resource_manifest.cmake")):
            for source, output, semantic, width, height, kind in re.findall(
                r'"([^|"]+)\|([^|"]+)\|([^|"]+)\|(\d+)\|(\d+)\|(\w+)"',
                    manifest.read_text(encoding="utf-8")):
                if kind != "SVG" or semantic.startswith("APP_IMAGE_WEATHER_"):
                    continue
                if not semantic.startswith("APP_IMAGE_UI_"):
                    self.assertIn(semantic, WINDOWS,
                                  f"{semantic} has no draw window recorded")

    def test_icon_ink_fits_every_draw_window(self) -> None:
        records = _records()
        self.assertGreater(len(records), 10)
        for semantic, path, width, height in records:
            with self.subTest(icon=semantic):
                image = _rasterize(path, width, height)
                bbox = _ink_bbox(image)
                self.assertIsNotNone(bbox, f"{path} renders nothing")
                for window in WINDOWS[semantic]:
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
