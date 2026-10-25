#!/usr/bin/env python3
"""Format only explicitly named, project-owned C/C++ files from the repo root."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]
SUFFIXES = {".c", ".h", ".cc", ".cpp", ".cxx", ".hh", ".hpp", ".hxx"}
EXCLUDED_PARTS = {".git", "managed_components", "XPowersLib", "esp-idf"}
CONFIG_OPTIONS = ["--options=none", "--project=none"]
OPTIONS = CONFIG_OPTIONS + [
    "--style=allman", "--indent=spaces=4", "--indent-preproc-block",
    "--pad-oper", "--pad-header", "--unpad-paren", "--suffix=none",
    "--align-pointer=name", "--lineend=linux", "--convert-tabs",
]


def select_sources(root: Path, names: list[str]) -> list[Path]:
    root = root.resolve()
    selected = []
    for name in names:
        path = root / name
        resolved = path.resolve()
        if not resolved.is_relative_to(root):
            raise ValueError(f"outside repository: {name}")
        relative = resolved.relative_to(root)
        if (any(part in EXCLUDED_PARTS or part == "build" or
                part.startswith(("build-", "cmake-build-"))
                for part in relative.parts) or
                resolved.name.startswith("mmap_generate_")):
            raise ValueError(f"third-party/generated path: {name}")
        # Do not let an alias silently redirect the task's explicit file set.
        if any(part.is_symlink() for part in (path, *path.parents)
               if part.is_relative_to(root)):
            raise ValueError(f"symlink path: {name}")
        if not resolved.is_file() or resolved.suffix not in SUFFIXES:
            raise ValueError(f"expected an existing C/C++ file: {name}")
        ignored = subprocess.run(
            ["git", "-C", str(resolved.parent), "check-ignore", "-q", "--",
             resolved.name], check=False)
        if ignored.returncode not in (0, 1):
            raise ValueError(f"cannot determine Git ownership: {name}")
        if ignored.returncode == 0:
            raise ValueError(f"ignored/generated file: {name}")
        if resolved not in selected:
            selected.append(resolved)
    return selected


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="read-only check")
    parser.add_argument("files", nargs="*", help="explicit repo-relative paths")
    args = parser.parse_args(argv)
    try:
        files = select_sources(ROOT, args.files)
        if not files:
            return 0
        version = subprocess.run(
            ["astyle", *CONFIG_OPTIONS, "--version"], check=True,
            capture_output=True, text=True)
        if not re.search(r"\bVersion 3\.6\.19\b", version.stdout + version.stderr):
            raise ValueError("AStyle 3.6.19 is required")
        options = OPTIONS + (["--dry-run", "--error-on-changes"] if args.check else [])
        return subprocess.run(
            ["astyle", *options, *(str(path) for path in files)], check=False
        ).returncode
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    raise SystemExit(main())
