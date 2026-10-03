#!/usr/bin/env python3
"""Run repository Python test groups from one deterministic entry point."""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

GROUPS = {
    "configuration": (ROOT / "tests" / "configuration", "test_*.py"),
    "docs": (ROOT / "tests" / "docs", "test_*.py"),
    "resources": (ROOT / "tests" / "resources", "test_*.py"),
    "display-tools": (ROOT / "tests" / "display", "test_*.py"),
    "sim-tools": (ROOT / "sim", "test_*.py"),
}


def _run_group(name: str) -> int:
    start = time.monotonic()
    source, pattern = GROUPS[name]
    command = [
        sys.executable,
        "-m",
        "unittest",
        "discover",
        "-s",
        str(source),
        "-p",
        pattern,
        "-v",
    ]
    result = subprocess.run(command, cwd=ROOT, check=False)
    elapsed = time.monotonic() - start
    status = "PASS" if result.returncode == 0 else "FAIL"
    print(f"[{status}] {name} duration={elapsed:.2f}s")
    return result.returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "groups",
        nargs="*",
        choices=[*GROUPS, "all"],
        default=["all"],
        help="test groups to run; default: all",
    )
    args = parser.parse_args()
    groups = list(GROUPS) if "all" in args.groups else args.groups

    failures = False
    for group in groups:
        failures = _run_group(group) != 0 or failures
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
