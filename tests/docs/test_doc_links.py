#!/usr/bin/env python3
"""Read-only check that repository Markdown links resolve to real paths."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


EXCLUDED_PARTS = {"managed_components", "build", ".git", "XPowersLib"}
# A target is either an angle-bracketed path (may contain spaces) or a bare
# token; an optional title in quotes may follow.
LINK = re.compile(r"\[[^\]]*\]\(\s*(<[^>]*>|[^)\s]+)(?:\s+\"[^\"]*\")?\s*\)")
IGNORED_SCHEMES = ("http://", "https://", "mailto:", "ftp://")
FENCE = re.compile(r"^\s*(```|~~~)")


class DocLinkTest(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(__file__).resolve().parents[2]

    def _documents(self):
        for path in sorted(self.root.rglob("*.md")):
            relative = path.relative_to(self.root)
            if any(part in EXCLUDED_PARTS for part in relative.parts):
                continue
            yield relative, path

    @staticmethod
    def _links(text: str):
        in_fence = False
        for line in text.splitlines():
            if FENCE.match(line):
                in_fence = not in_fence
                continue
            if in_fence:
                continue
            for raw in LINK.findall(line):
                if raw.startswith("<") and raw.endswith(">"):
                    raw = raw[1:-1]
                yield raw

    def test_relative_markdown_links_resolve(self) -> None:
        failures = []
        for relative, path in self._documents():
            base = path.parent
            for target in self._links(path.read_text(encoding="utf-8")):
                if not target or target.startswith(IGNORED_SCHEMES) or \
                        target.startswith("#"):
                    continue
                if target.startswith("/") or "://" in target:
                    continue
                local = target.split("#", 1)[0]
                if not local:
                    continue
                if not (base / local).exists():
                    failures.append(f"{relative}: broken link -> {target}")
        self.assertEqual(failures, [], "\n".join(failures))


if __name__ == "__main__":
    unittest.main()