#!/usr/bin/env python3
"""Check a saved design-skill output against an eval case's must/must_not list."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

TARGET_ID = re.compile(r"\bpanel_\d+x\d+(?:_[a-z0-9]+)*\b", re.IGNORECASE)
CRITICAL = re.compile(
    r"(?:#[0-9A-Fa-f]{6}|AMOLED|PLUME|AZURE|\b368\b|\b448\b)",
    re.IGNORECASE)


def check_output(case: dict, output: str) -> dict:
    missing = [value for value in case.get("must", []) if value not in output]
    forbidden = [value for value in case.get("must_not", []) if value in output]
    # A target id may legitimately contain 368/448; it is not a design input.
    design_input = TARGET_ID.sub("", output)
    critical = sorted(set(CRITICAL.findall(design_input)))
    ok = not missing and not forbidden and not critical
    return {
        "case": case.get("id"),
        "ok": ok,
        "missing": missing,
        "forbidden": forbidden,
        "critical": critical,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args(argv)
    case = json.loads(Path(args.case).read_text(encoding="utf-8"))
    output = Path(args.output).read_text(encoding="utf-8")
    result = check_output(case, output)
    print(json.dumps(result, indent=2))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
