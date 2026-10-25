#!/usr/bin/env python3
"""Read-only check that repository Markdown links resolve to real paths."""

from __future__ import annotations

import re
import subprocess
import unittest
from pathlib import Path


EXCLUDED_PARTS = {"managed_components", "build", ".git", "XPowersLib"}
# A target is either an angle-bracketed path (may contain spaces) or a bare
# token; an optional title in quotes may follow.
LINK = re.compile(r"\[[^\]]*\]\(\s*(<[^>]*>|[^)\s]+)(?:\s+\"[^\"]*\")?\s*\)")
IGNORED_SCHEMES = ("http://", "https://", "mailto:", "ftp://")
FENCE = re.compile(r"^\s*(```|~~~)")


def documents(root: Path):
    """Enumerate owned tracked/new Markdown, including initialized submodules."""
    def owned(path):
        return not any(part in EXCLUDED_PARTS or
                       part.startswith(("build-", "cmake-build-"))
                       for part in path.parts)

    def visit(repository):
        output = subprocess.check_output(
            ["git", "-C", str(repository), "ls-files", "-z", "--cached",
             "--others", "--exclude-standard", "--", "*.md"])
        for name in sorted(set(output.decode().split("\0")) - {""}):
            path = repository / name
            relative = path.relative_to(root)
            if owned(relative) and path.is_file() and not path.is_symlink():
                yield relative, path
        index = subprocess.check_output(
            ["git", "-C", str(repository), "ls-files", "--stage", "-z"])
        for entry in index.decode().split("\0"):
            if not entry.startswith("160000 "):
                continue
            submodule = repository / entry.split("\t", 1)[1]
            if not owned(submodule.relative_to(root)) or not submodule.is_dir():
                continue
            # An uninitialized directory would otherwise resolve to the parent repo.
            top = subprocess.run(
                ["git", "-C", str(submodule), "rev-parse", "--show-toplevel"],
                capture_output=True, text=True, check=False)
            if top.returncode == 0 and Path(top.stdout.strip()).resolve() == submodule.resolve():
                yield from visit(submodule)

    yield from visit(root.resolve())


class DocLinkTest(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(__file__).resolve().parents[2]

    def _documents(self):
        yield from documents(self.root)

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
