#!/usr/bin/env python3
"""Cross-check the board, product, App catalog and document contracts."""

from __future__ import annotations

import json
import re
import unittest
from pathlib import Path

import yaml


ROOT = Path(__file__).resolve().parents[2]
APPS = ROOT / "layers" / "apps"
BSP = ROOT / "layers" / "bsp"
PRODUCTS = ROOT / "products"
DESIGN_TARGETS = ROOT / "doc" / "design" / "targets"


def _cmake_list(text: str, opener: str) -> list[str]:
    start = text.index(opener) + len(opener)
    body = text[start:]
    end = body.index(")")
    body = body[:end]
    return [token for token in re.findall(r"[A-Za-z0-9_./+:-]+", body)]


def _catalog_ids() -> list[str]:
    return _cmake_list(
        (APPS / "app_catalog.cmake").read_text(encoding="utf-8"),
        "set(MT_APP_CATALOG_IDS")


def _app_manifest(app_id: str) -> dict[str, object]:
    app_dir = "common" if app_id == "common" else f"{app_id}_app"
    path = APPS / app_dir / "app.cmake"
    text = path.read_text(encoding="utf-8")
    result: dict[str, object] = {"dir": app_dir, "path": path}
    for field in ("MT_APP_ID", "MT_APP_RESET_HOOK"):
        match = re.search(rf"set\({field}\s+([^\s)]+)\)", text)
        result[field] = match.group(1) if match else ""
    for field in ("MT_APP_SOURCES", "MT_APP_INCLUDE_DIRS",
                  "MT_APP_PRIVATE_REQUIRES", "MT_APP_CAPABILITIES"):
        result[field] = _cmake_list(text, f"set({field}")
    result["MT_APP_REQUIRED"] = "set(MT_APP_REQUIRED TRUE)" in text
    result["MT_APP_HIDDEN"] = "set(MT_APP_HIDDEN TRUE)" in text
    result["MT_APP_RESOURCE_MANIFEST"] = (
        "resource_manifest.cmake" in text)
    return result


def _product(path: Path) -> dict[str, object]:
    text = path.read_text(encoding="utf-8")
    result: dict[str, object] = {"path": path}
    for field in ("MT_PRODUCT_ID", "MT_PRODUCT_BOARD",
                  "MT_PRODUCT_DESIGN_TARGET"):
        match = re.search(rf'set\({field}\s+"([^"]+)"\)', text)
        result[field] = match.group(1) if match else ""
    for field in ("MT_PRODUCT_HAL_CAPABILITIES", "MT_PRODUCT_APPS",
                  "MT_PRODUCT_REQUIRED_APPS"):
        result[field] = _cmake_list(text, f"set({field}")
    return result


def _boards() -> dict[str, dict[str, object]]:
    boards: dict[str, dict[str, object]] = {}
    for path in sorted(BSP.glob("waveshare/*/board.cmake")):
        text = path.read_text(encoding="utf-8")
        board_id = re.search(r'set\(MT_BOARD_ID\s+"([^"]+)"\)', text).group(1)
        boards[board_id] = {
            "path": path,
            "name": re.search(r'set\(MT_BOARD_NAME\s+"([^"]+)"\)',
                              text).group(1),
            "default_product": re.search(
                r'set\(MT_BOARD_DEFAULT_PRODUCT\s+"([^"]+)"\)', text).group(1),
            "documents": re.search(
                r'set\(MT_BOARD_DOCUMENTS\s+"([^"]+)"\)', text).group(1),
            "capabilities": _cmake_list(text,
                                        "set(MT_BOARD_HAL_CAPABILITIES"),
        }
    return boards


class ProductCatalogTest(unittest.TestCase):
    def setUp(self) -> None:
        self.catalog = _catalog_ids()
        self.boards = _boards()
        self.products = {
            product["MT_PRODUCT_ID"]: product
            for product in (_product(path) for path in
                            sorted(PRODUCTS.glob("*/product.cmake")))
        }

    def test_every_app_directory_is_catalogued_and_complete(self) -> None:
        on_disk = sorted(
            [path.name for path in APPS.glob("*_app") if path.is_dir()] +
            ["common"])
        catalog_dirs = sorted(
            "common" if app_id == "common" else f"{app_id}_app"
            for app_id in self.catalog)
        self.assertEqual(on_disk, catalog_dirs)
        for app_id in self.catalog:
            with self.subTest(app=app_id):
                manifest = _app_manifest(app_id)
                self.assertEqual(manifest["MT_APP_ID"], app_id)
                self.assertTrue(manifest["MT_APP_SOURCES"])
                for source in manifest["MT_APP_SOURCES"]:
                    self.assertTrue(
                        (APPS / manifest["dir"] / source).is_file(),
                        f"{app_id}: missing {source}")
                for include_dir in manifest["MT_APP_INCLUDE_DIRS"]:
                    self.assertTrue(
                        (APPS / manifest["dir"] / include_dir).is_dir(),
                        f"{app_id}: missing include {include_dir}")
                if manifest["MT_APP_RESOURCE_MANIFEST"]:
                    self.assertTrue(
                        (APPS / manifest["dir"] /
                         "resource_manifest.cmake").is_file())

    def test_products_reference_known_apps_boards_and_targets(self) -> None:
        for product_id, product in self.products.items():
            with self.subTest(product=product_id):
                self.assertIn(product["MT_PRODUCT_BOARD"], self.boards)
                self.assertTrue((DESIGN_TARGETS /
                                 f"{product['MT_PRODUCT_DESIGN_TARGET']}.json")
                                .is_file())
                apps = product["MT_PRODUCT_APPS"]
                self.assertEqual(len(apps), len(set(apps)))
                for app_id in apps:
                    self.assertIn(app_id, self.catalog)
                for required in product["MT_PRODUCT_REQUIRED_APPS"]:
                    self.assertIn(required, apps)
                for app_id in apps:
                    manifest = _app_manifest(app_id)
                    if manifest["MT_APP_REQUIRED"]:
                        self.assertIn(app_id, apps)

    def test_app_capabilities_are_available_on_each_product(self) -> None:
        for product_id, product in self.products.items():
            board = self.boards[product["MT_PRODUCT_BOARD"]]
            available = set(board["capabilities"]) | set(
                product["MT_PRODUCT_HAL_CAPABILITIES"])
            for app_id in product["MT_PRODUCT_APPS"]:
                manifest = _app_manifest(app_id)
                missing = set(manifest["MT_APP_CAPABILITIES"]) - available
                self.assertEqual(
                    missing, set(),
                    f"{product_id} selects {app_id} without {sorted(missing)}")

    def test_board_defaults_and_document_registries_exist(self) -> None:
        for board_id, board in self.boards.items():
            with self.subTest(board=board_id):
                self.assertIn(board["default_product"], self.products)
                documents = board["path"].parent / board["documents"]
                self.assertTrue(documents.is_file(), documents)
                data = yaml.safe_load(documents.read_text(encoding="utf-8"))
                self.assertEqual(data["schema_version"], 1)
                self.assertEqual(data["board"], board_id)
                self.assertTrue(data["documents"])
                for entry in data["documents"]:
                    for field in ("id", "title", "revision", "url",
                                  "retrieved", "sha256", "redistribution",
                                  "scope"):
                        self.assertIn(field, entry)
                    self.assertTrue(entry["url"].startswith("https://"))
                    self.assertIn(entry["redistribution"],
                                  {"link-only", "redistributable"})
                    self.assertTrue(entry["scope"])
                    if entry["sha256"] != "N/A":
                        self.assertRegex(entry["sha256"], r"^[0-9a-f]{64}$")

    def test_optional_entry_points_are_guarded(self) -> None:
        home = (APPS / "home_app" / "src" / "home_app.c").read_text(
            encoding="utf-8").splitlines()
        for app_id in ("WEATHER", "CLOCK", "CALCULATOR", "CALENDAR"):
            needle = f"APP_MANAGER_ID_{app_id}"
            indexes = [index for index, line in enumerate(home)
                       if needle in line and "_home_card" in line]
            self.assertTrue(indexes, f"home has no {app_id} card")
            for index in indexes:
                window = "\n".join(home[max(0, index - 10):index + 1])
                self.assertIn(
                    f"app_manager_builtin_has_app({needle})", window,
                    f"home {app_id} card is not guarded")

        settings = (APPS / "settings_app" / "src" / "settings_app.c").read_text(
            encoding="utf-8").splitlines()
        for text, app_id in (('"连接向导"', "SETUP"),
                             ("APP_MANAGER_ID_DIAGNOSTICS", "DIAGNOSTICS")):
            indexes = [index for index, line in enumerate(settings)
                       if text in line]
            self.assertTrue(indexes, f"settings has no {app_id} entry")
            for index in indexes:
                window = "\n".join(settings[max(0, index - 4):index + 1])
                self.assertIn(
                    f"app_manager_builtin_has_app(APP_MANAGER_ID_{app_id})",
                    window, f"settings {app_id} entry is not guarded")

        storage = (APPS / "settings_app" / "src" /
                   "settings_storage_page.c").read_text(
                       encoding="utf-8").splitlines()
        indexes = [index for index, line in enumerate(storage)
                   if '"文件管理"' in line]
        self.assertTrue(indexes, "storage has no Files entry")
        for index in indexes:
            window = "\n".join(storage[max(0, index - 4):index + 1])
            self.assertIn("app_manager_builtin_has_app(APP_MANAGER_ID_FILES)",
                          window, "storage Files entry is not guarded")

    def test_minimal_product_is_the_required_core(self) -> None:
        minimal = self.products["minimal"]
        self.assertEqual(set(minimal["MT_PRODUCT_APPS"]),
                         {"common", "home", "menu", "settings"})
        self.assertTrue(minimal["MT_PRODUCT_REQUIRED_APPS"])

    def test_product_and_app_ids_have_no_duplicates(self) -> None:
        self.assertEqual(len(self.catalog), len(set(self.catalog)))
        self.assertEqual(len(self.products), len(set(self.products)))
        for product in self.products.values():
            self.assertTrue(product["MT_PRODUCT_ID"])

    def test_design_targets_match_board_display_profiles(self) -> None:
        for product_id, product in self.products.items():
            with self.subTest(product=product_id):
                board = self.boards[product["MT_PRODUCT_BOARD"]]
                target = json.loads((DESIGN_TARGETS /
                                     f"{product['MT_PRODUCT_DESIGN_TARGET']}.json")
                                    .read_text(encoding="utf-8"))
                profile = (board["path"].parent / "board_display_profile.h")
                text = profile.read_text(encoding="utf-8")
                width = int(re.search(
                    r"#define BOARD_LCD_HOR_RES\s+\((\d+)U\)", text).group(1))
                height = int(re.search(
                    r"#define BOARD_LCD_VER_RES\s+\((\d+)U\)", text).group(1))
                self.assertEqual((target["display"]["width"],
                                  target["display"]["height"]),
                                 (width, height))


if __name__ == "__main__":
    unittest.main()
