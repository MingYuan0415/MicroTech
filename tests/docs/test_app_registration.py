#!/usr/bin/env python3
"""Guard that Apps are registered through the catalog and product manifests.

Adding an App is one catalog entry plus its `app.cmake`; firmware, simulator
and cross-layer tests consume the product expansion instead of maintaining
their own source lists.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
APPS_DIR = PROJECT_ROOT / "layers" / "apps"
CATALOG = APPS_DIR / "app_catalog.cmake"


def _catalog_ids() -> list[str]:
    text = CATALOG.read_text(encoding="utf-8")
    start = text.index("set(MT_APP_CATALOG_IDS") + len("set(MT_APP_CATALOG_IDS")
    body = text[start:text.index(")", start)]
    return re.findall(r"[A-Za-z0-9_]+", body)


def _app_sources() -> dict[str, set[str]]:
    sources: dict[str, set[str]] = {}
    for app_id in _catalog_ids():
        app_dir = "common" if app_id == "common" else f"{app_id}_app"
        text = (APPS_DIR / app_dir / "app.cmake").read_text(encoding="utf-8")
        start = text.index("set(MT_APP_SOURCES") + len("set(MT_APP_SOURCES")
        body = text[start:text.index(")", start)]
        sources[app_id] = set(re.findall(r"[A-Za-z0-9_./+-]+\.c", body))
    return sources


class AppRegistrationTest(unittest.TestCase):
    def test_every_app_source_is_catalogued(self) -> None:
        catalog_sources: set[str] = set()
        for app_id, sources in _app_sources().items():
            app_dir = "common" if app_id == "common" else f"{app_id}_app"
            for source in sources:
                catalog_sources.add(f"{app_dir}/{source}")
        on_disk = {
            str(path.relative_to(APPS_DIR))
            for path in sorted(APPS_DIR.glob("*/src/*.c"))
        }
        self.assertTrue(on_disk)
        self.assertEqual(sorted(on_disk - catalog_sources), [],
                         "sources missing from an app.cmake")
        self.assertEqual(sorted(catalog_sources - on_disk), [],
                         "app.cmake lists a missing source")

    def test_consumers_use_product_expansion(self) -> None:
        for consumer in (PROJECT_ROOT / "sim" / "CMakeLists.txt",
                         PROJECT_ROOT / "tests" / "integration" /
                         "CMakeLists.txt"):
            text = consumer.read_text(encoding="utf-8")
            self.assertIn("products.cmake", text,
                          f"{consumer} must include the product resolver")
            self.assertIn("mt_product_expand_apps", text,
                          f"{consumer} must use the product expansion")
            self.assertNotIn("app_sources.cmake", text,
                             f"{consumer} still reads a per-layer app list")

    def test_cross_layer_inventory_matches_catalog(self) -> None:
        test = (PROJECT_ROOT / "tests" / "integration" /
                "test_cross_layer.c").read_text(encoding="utf-8")
        ids_header = (PROJECT_ROOT / "layers" / "app_manager" / "app_core" /
                      "include" / "app_manager_ids.h").read_text(
                          encoding="utf-8")
        macros = dict(re.findall(
            r'#define\s+(APP_MANAGER_ID_[A-Z_]+)\s+"([^"]+)"', ids_header))
        registered = set()
        for descriptor in APPS_DIR.rglob("*_app.c"):
            registered.update(re.findall(
                r"APP_MANAGER_APP_EXPORT(?:_META)?\(\s*[^,]+,\s*[^,]+,\s*"
                r"[^,]+,\s*(APP_MANAGER_ID_[A-Z_]+)",
                descriptor.read_text(encoding="utf-8")))
        registered &= set(macros)
        for macro in registered:
            self.assertIn(f"#if defined(MT_PRODUCT_HAS_",
                          test)
            self.assertIn(macro, test,
                          f"cross-layer inventory omits {macro}")


if __name__ == "__main__":
    unittest.main()
