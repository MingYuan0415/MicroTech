#!/usr/bin/env python3
"""Guard that the app layer is registered in exactly one place.

Adding an App used to require hand-editing a source list in three projects, a
descriptor count in the cross-layer suite and several counts in the simulator
README, each of which drifted silently. The source list now lives only in
`layers/apps/app_sources.cmake`; these checks keep it that way and keep the one
remaining count derived rather than remembered.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
APPS_DIR = PROJECT_ROOT / "layers" / "apps"
SHARED_LIST = APPS_DIR / "app_sources.cmake"


def _listed_sources():
    text = SHARED_LIST.read_text(encoding="utf-8")
    lines = text[text.index("set(APP_SRCS"):].splitlines()
    body = []
    for line in lines[1:]:
        body.append(line)
        if line.rstrip().endswith(")"):
            break
    return re.findall(r'"([^"]+)"', "\n".join(body))


class AppRegistrationTest(unittest.TestCase):
    def test_every_app_source_is_listed(self) -> None:
        """A new App or page file must appear in the shared list."""
        listed = set(_listed_sources())
        on_disk = set()
        for source in sorted(APPS_DIR.glob("*/src/*.c")):
            on_disk.add(str(source.relative_to(APPS_DIR)))
        self.assertTrue(on_disk)
        self.assertEqual(sorted(on_disk - listed), [],
                         "sources missing from app_sources.cmake")
        self.assertEqual(sorted(listed - on_disk), [],
                         "app_sources.cmake lists a missing file")

    def test_consumers_do_not_redeclare_app_sources(self) -> None:
        """Sim and integration must consume the shared list, never copy it."""
        offenders = []
        for consumer in (PROJECT_ROOT / "sim" / "CMakeLists.txt",
                         PROJECT_ROOT / "tests" / "integration" /
                         "CMakeLists.txt"):
            text = consumer.read_text(encoding="utf-8")
            self.assertIn("app_sources.cmake", text,
                          f"{consumer} must include the shared app source list")
            for line in text.splitlines():
                stripped = line.strip().strip('"')
                if stripped.startswith("${APPS}/") and \
                        re.match(r".+_app/src/|common/src/", stripped):
                    offenders.append(f"{consumer.name}: {stripped}")
        self.assertEqual(offenders, [],
                         "app sources re-declared outside app_sources.cmake")

    def test_cross_layer_inventory_matches_descriptors(self) -> None:
        """The cross-layer suite's App inventory matches the registered Apps."""
        test = (PROJECT_ROOT / "tests" / "integration" /
                "test_cross_layer.c").read_text(encoding="utf-8")
        self.assertNotRegex(
            test, r"#define BUILTIN_APP_COUNT\s+\d",
            "derive the App count from the inventory list, not a literal")
        declared = re.search(
            r"s_expected_app_ids\[\]\s*=\s*\{(.*?)\}", test, re.S)
        self.assertIsNotNone(declared)
        listed_macros = set(re.findall(r"APP_MANAGER_ID_[A-Z_]+",
                                       declared.group(1)))

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

        self.assertEqual(sorted(listed_macros), sorted(registered),
                         "cross-layer App inventory is out of date")


if __name__ == "__main__":
    unittest.main()