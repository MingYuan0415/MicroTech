#!/usr/bin/env python3
"""Validate the fixed weather image contract packaged in res_fs."""

from pathlib import Path
import struct
import unittest


PROJECT_ROOT = Path(__file__).resolve().parents[2]
RESOURCE_DIR = PROJECT_ROOT / "main" / "res_fs"
CONDITIONS = (
    "clear_day",
    "clear_night",
    "partly_day",
    "partly_night",
    "cloudy",
    "overcast",
    "drizzle",
    "rain",
    "heavy_rain",
    "thunder",
    "hail",
    "freezing_rain",
    "snow",
    "heavy_snow",
    "sleet",
    "fog",
    "haze",
    "hot",
    "cold",
    "unknown",
)


def _png_header(path: Path) -> tuple[int, int, int, int]:
    with path.open("rb") as image:
        signature = image.read(8)
        length = struct.unpack(">I", image.read(4))[0]
        chunk_type = image.read(4)
        payload = image.read(length)
    if signature != b"\x89PNG\r\n\x1a\n" or chunk_type != b"IHDR":
        raise AssertionError(f"{path.name} is not a PNG with an IHDR header")
    width, height, bit_depth, color_type = struct.unpack(">IIBB", payload[:10])
    return width, height, bit_depth, color_type


class WeatherAssetTest(unittest.TestCase):
    def test_complete_rgba_resource_set(self) -> None:
        expected = {"weather_app.png"}
        for condition in CONDITIONS:
            expected.add(f"weather_{condition}_112.png")
            expected.add(f"weather_{condition}_40.png")
        actual = {path.name for path in RESOURCE_DIR.glob("weather_*.png")}
        self.assertEqual(expected, actual)

    def test_dimensions_and_alpha(self) -> None:
        expected_sizes = {"weather_app.png": (64, 64)}
        for condition in CONDITIONS:
            expected_sizes[f"weather_{condition}_112.png"] = (112, 112)
            expected_sizes[f"weather_{condition}_40.png"] = (40, 40)
        for filename, expected_size in expected_sizes.items():
            with self.subTest(filename=filename):
                width, height, bit_depth, color_type = _png_header(
                    RESOURCE_DIR / filename
                )
                self.assertEqual(expected_size, (width, height))
                self.assertEqual(8, bit_depth)
                self.assertEqual(6, color_type, "PNG must use RGBA color type")


if __name__ == "__main__":
    unittest.main()
