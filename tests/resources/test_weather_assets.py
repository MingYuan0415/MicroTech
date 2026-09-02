#!/usr/bin/env python3
"""Validate the SVG-owned weather image set vendored from QWeather Icons."""

from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET


PROJECT_ROOT = Path(__file__).resolve().parents[2]
RESOURCE_DIR = PROJECT_ROOT / "layers" / "apps" / "weather_app" / "assets"
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


class WeatherAssetTest(unittest.TestCase):
    def test_complete_svg_resource_set(self) -> None:
        expected = {"weather_app.svg"}
        for condition in CONDITIONS:
            expected.add(f"weather_{condition}.svg")
        actual = {path.name for path in RESOURCE_DIR.glob("weather_*.svg")}
        self.assertEqual(expected, actual)

    def test_no_raster_sources_remain(self) -> None:
        self.assertEqual([], list(RESOURCE_DIR.glob("weather_*.png")))

    def test_svg_sources_are_final_colored(self) -> None:
        names = ["weather_app.svg"] + [f"weather_{c}.svg" for c in CONDITIONS]
        for name in names:
            with self.subTest(name=name):
                text = (RESOURCE_DIR / name).read_text(encoding="utf-8")
                self.assertNotIn("currentColor", text)
                root = ET.fromstring(text[text.index("<svg"):])
                self.assertTrue(root.get("viewBox"), name)

    def test_manifest_declares_both_export_sizes(self) -> None:
        text = (RESOURCE_DIR.parent / "resource_manifest.cmake").read_text()
        for condition in CONDITIONS:
            with self.subTest(condition=condition):
                for size, suffix in ((112, "MAIN"), (40, "SMALL")):
                    pattern = (
                        r"weather_{c}\.svg\|weather_{c}_{s}\.png\|"
                        r"APP_IMAGE_WEATHER_{C}_{X}\|{s}\|{s}\|SVG"
                    )
                    self.assertRegex(
                        text,
                        pattern.format(
                            c=condition, C=condition.upper(), s=size, X=suffix
                        ),
                    )

    def test_qweather_license_shipped(self) -> None:
        license_text = (
            RESOURCE_DIR / "qweather-icons-LICENSE.txt"
        ).read_text(encoding="utf-8")
        self.assertIn("QWeather", license_text)


if __name__ == "__main__":
    unittest.main()
