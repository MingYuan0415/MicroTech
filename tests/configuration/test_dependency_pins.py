#!/usr/bin/env python3
"""Read-only checks that component and host dependencies stay pinned."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

import yaml


# `firmware/` holds standalone ESP-IDF projects with their own
# dependencies.lock (for example the C6 coprocessor), not root components.
EXCLUDED_PARTS = {"managed_components", "build", ".git", ".cache", "firmware"}
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
        self.locks = self._load_locks()

    def _load_locks(self):
        paths = sorted(self.root.glob("dependencies.lock.*"))
        if not paths:
            legacy = self.root / "dependencies.lock"
            if legacy.is_file():
                paths = [legacy]
        self.assertTrue(
            paths,
            "no dependencies.lock.<target> or dependencies.lock found",
        )
        locks = []
        for path in paths:
            data = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
            target = str(data.get("target") or "").strip()
            self.assertTrue(target, f"{path.name}: missing target")
            locks.append((path, data, target))
        return locks

    def _condition_matches(self, condition: str, target: str) -> bool:
        exact = re.fullmatch(r"target\s*==\s*([A-Za-z0-9_]+)", condition)
        if exact:
            return exact.group(1) == target
        negated = re.fullmatch(r"target\s*!=\s*([A-Za-z0-9_]+)", condition)
        if negated:
            return negated.group(1) != target
        member = re.fullmatch(r"target\s+in\s*\[([^\]]*)\]", condition)
        if member:
            names = {item.strip() for item in member.group(1).split(",")
                     if item.strip()}
            return target in names
        # A rule the checker cannot evaluate is treated as applicable so the
        # dependency still has to be locked for the lock's target.
        return True

    def _dependency_applies(self, spec, target: str) -> bool:
        if not isinstance(spec, dict):
            return True
        rules = spec.get("rules")
        if not rules:
            return True
        for rule in rules:
            condition = str((rule or {}).get("if", "")).strip()
            if not condition or self._condition_matches(condition, target):
                return True
        return False

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
        manifests = list(self._manifests())
        failures = []
        for lock_path, lock, target in self.locks:
            locked = set((lock.get("dependencies") or {}).keys())
            self.assertTrue(
                locked,
                f"{lock_path.name} did not parse any components",
            )
            # The component manager accepts both fully qualified and bare
            # names, so match on the final path component as well.
            locked_short = {name.rsplit("/", 1)[-1] for name in locked}
            for relative, path in manifests:
                for name, spec in self._dependencies(relative, path).items():
                    if name == "idf":
                        continue
                    if isinstance(spec, dict) and "path" in spec:
                        continue
                    if not self._dependency_applies(spec, target):
                        continue
                    if (name in locked or
                            name.rsplit("/", 1)[-1] in locked_short):
                        continue
                    failures.append(
                        f"{relative}: {name} is absent from {lock_path.name} "
                        f"(target {target})"
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