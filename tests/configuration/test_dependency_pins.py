#!/usr/bin/env python3
"""Read-only checks that component and host dependencies stay pinned."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

import yaml


EXCLUDED_PARTS = {"managed_components", "build", ".git"}
WILDCARD_VERSIONS = {"", "*"}
# name, optional [extras], exact ==, version without a second '=', optional
# PEP 508 environment marker. Rejects 'foo==1.0==bar' and accepts inline
# comments (stripped before matching) and extras.
REQUIREMENT_PIN = re.compile(
    r"^[A-Za-z0-9][A-Za-z0-9._-]*(\[[A-Za-z0-9_,.\- ]+\])?"
    r"[ \t]*==[ \t]*[^\s;=]+([ \t]*;.*)?$")


class DependencyPinTest(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(__file__).resolve().parents[2]
        self.lock = yaml.safe_load(
            (self.root / "dependencies.lock").read_text(encoding="utf-8")
        ) or {}

    def _manifests(self):
        for path in sorted(self.root.rglob("idf_component.yml")):
            relative = path.relative_to(self.root)
            if any(part in EXCLUDED_PARTS for part in relative.parts):
                continue
            yield relative, path

    def _dependencies(self, relative: Path, path: Path) -> dict:
        data = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
        dependencies = data.get("dependencies") or {}
        self.assertIsInstance(
            dependencies, dict, f"{relative}: dependencies must be a mapping"
        )
        return dependencies

    def test_component_manifests_have_no_wildcard_versions(self) -> None:
        failures = []
        for relative, path in self._manifests():
            for name, spec in self._dependencies(relative, path).items():
                if name == "idf":
                    continue
                if isinstance(spec, dict):
                    if "path" in spec:
                        continue
                    version = str(spec.get("version", "")).strip()
                elif isinstance(spec, str):
                    version = spec.strip()
                else:
                    failures.append(f"{relative}: {name} has unsupported spec")
                    continue
                if version in WILDCARD_VERSIONS:
                    failures.append(
                        f"{relative}: {name} uses a wildcard version"
                    )
        self.assertEqual(failures, [], "\n".join(failures))

    def test_registry_dependencies_exist_in_lock(self) -> None:
        locked = set((self.lock.get("dependencies") or {}).keys())
        self.assertTrue(locked, "dependencies.lock did not parse any components")
        # The component manager accepts both fully qualified and bare names,
        # so match on the final path component as well.
        locked_short = {name.rsplit("/", 1)[-1] for name in locked}
        failures = []
        for relative, path in self._manifests():
            for name, spec in self._dependencies(relative, path).items():
                if name == "idf":
                    continue
                if isinstance(spec, dict) and "path" in spec:
                    continue
                if name in locked or name.rsplit("/", 1)[-1] in locked_short:
                    continue
                failures.append(
                    f"{relative}: {name} is absent from dependencies.lock"
                )
        self.assertEqual(failures, [], "\n".join(failures))

    def test_host_requirements_are_exactly_pinned(self) -> None:
        failures = []
        for path in sorted(self.root.glob("requirements*.txt")):
            for number, raw in enumerate(
                    path.read_text(encoding="utf-8").splitlines(), 1):
                line = raw.split("#", 1)[0].strip()
                if not line or line.startswith("-"):
                    continue
                if not REQUIREMENT_PIN.match(line):
                    failures.append(
                        f"{path.name}:{number}: not exactly pinned: {line}"
                    )
        self.assertEqual(failures, [], "\n".join(failures))


if __name__ == "__main__":
    unittest.main()