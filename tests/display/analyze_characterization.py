#!/usr/bin/env python3
"""Analyze the production C_EXT two-load, five-effect characterization log."""
from __future__ import annotations

import argparse
import json
from dataclasses import asdict
from pathlib import Path

import analyze_c_ext_stress as stress
import display_log_utils as logs


def analyze(path: Path) -> stress.StressAnalysis:
    report = stress.StressAnalysis(path)
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as error:
        report.validation_errors.append(str(error))
        return report
    stress._validate_config(lines, report)
    stress._validate_display(lines, report)
    for marker in logs.FATAL_MARKERS:
        if any(marker in line.lower() for line in lines):
            report.gate_failures.append(f"fatal marker: {marker}")
    for marker in logs.RESTART_MARKERS:
        if sum(marker in line for line in lines) > 1:
            report.gate_failures.append(f"repeated startup: {marker}")
    for load in logs.LOADS:
        for effect in logs.EFFECTS:
            records = [r for r in logs.records(lines, "display perf ")
                       if r.get("load") == load and r.get("effect") == effect]
            context = f"effect {load}/{effect}"
            if len(records) != 1:
                report.validation_errors.append(f"{context}: expected one record")
                continue
            record = records[0]
            stress._require_result(record, context, report)
            for name in ("active_frames", "active_ms", "intervals", "complete",
                         "snapshot_prepare_count"):
                stress._positive(record, name, context, report)
            for name in ("cancel", "snapshot_fallbacks"):
                stress._equals(record, name, 0, context, report)
            for name, maximum in (("p95_us", 50000), ("p99_us", 66667),
                                  ("max_us", 100000),
                                  ("snapshot_prepare_p95_us", 100000)):
                value = stress._positive(record, name, context, report)
                if value is not None and value > maximum:
                    report.gate_failures.append(f"{context}: {name}={value}")
            fps = stress._positive(record, "avg_fps_x100", context, report)
            if fps is not None and fps < 2500:
                report.gate_failures.append(f"{context}: FPS below FLOOR")
            if record.get("snapshot") != "enabled":
                report.gate_failures.append(f"{context}: snapshot disabled")
        profiles = [r for r in logs.records(lines, "display profile ")
                    if r.get("load") == load]
        memory = [r for r in logs.records(lines, "display memory ")
                  if r.get("load") == load]
        if len(profiles) != 1 or len(memory) != 1:
            report.validation_errors.append(f"{load}: missing/duplicate profile or memory")
            continue
        profile = profiles[0]
        stress._positive(profile, "samples", load, report)
        for name in ("sample_err", "submit_fail", "dma_fail", "snapshot_fallbacks"):
            stress._equals(profile, name, 0, load, report)
        if profile.get("diagnostics") != "PASS":
            report.gate_failures.append(f"{load}: diagnostics failed")
        record = memory[0]
        for name in logs.MEMORY_INTEGER_FIELDS[:6]:
            stress._positive(record, name, load, report)
        for name in ("render_task", "render_stack_psram", "render_core"):
            stress._equals(record, name, 1, load, report)
        for name, minimum in (("min_dma_largest", 14720), ("render_stack_hwm", 4096)):
            value = stress._integer(record, name, load, report)
            if value is not None and value < minimum:
                report.gate_failures.append(f"{load}: {name}={value}")
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    report = analyze(args.log)
    print(json.dumps(asdict(report) | {"path": str(report.path), "status": report.status},
                     indent=2))
    return 0 if report.status == "PASS" else 2


if __name__ == "__main__":
    raise SystemExit(main())
