#!/usr/bin/env python3
"""Validate LVGL internal-RAM candidate characterization logs."""

from __future__ import annotations

import argparse
import json
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Sequence

import display_log_utils as log_utils
import lvgl_ram_profiles as ram_profiles


DMA_LARGEST_MINIMUM = 14_720
SNAPSHOT_PREPARE_P95_MAXIMUM_US = 100_000
RENDER_STACK_HIGH_WATER_MINIMUM = 4_096
C_INTERNAL_GAIN_MINIMUM = 48 * 1024
FALLBACK_RECLAIM_PERCENT = 75

FIXED_CONFIG = {
    "bus_hz": 40_000_000,
    "draw_rows": 60,
    "dma_rows": 10,
    "dma_max_full_rows": 44,
    "queue": 2,
    "direct": 0,
    "te": 0,
    "tcp_payload": 5760,
    "tcp_prio": 2,
    "lifecycle_log": 0,
}
FIXED_CONFIG_STRINGS = {
    "transport": "qspi",
    "color": "RGB565",
    "snapshot": "enabled",
    "load_profile": "full",
}
MEMORY_FIELDS = log_utils.MEMORY_INTEGER_FIELDS
FATAL_MARKERS = log_utils.FATAL_MARKERS + (
    "lvgl task psram allocation failed",
)


@dataclass
class RamRunReport:
    profile: str
    path: Path
    performance: str | None = None
    minimum_internal_free: int | None = None
    minimum_internal_largest: int | None = None
    minimum_dma_largest: int | None = None
    minimum_render_stack_high_water: int | None = None
    render_task_found: bool | None = None
    render_task_stack_in_psram: bool | None = None
    render_task_core_id: int | None = None
    internal_gain: int | None = None
    validation_errors: list[str] = field(default_factory=list)
    gate_failures: list[str] = field(default_factory=list)

    @property
    def status(self) -> str:
        if self.validation_errors:
            return "INVALID"
        if self.gate_failures:
            return "FAIL"
        return "PASS"


@dataclass
class RamAnalysis:
    reports: dict[str, RamRunReport]
    selected: str | None
    reasons: list[str]

    @property
    def valid(self) -> bool:
        return not any(report.validation_errors
                       for report in self.reports.values())


def _records(lines: Sequence[str], marker: str) -> list[dict[str, str]]:
    return log_utils.records(lines, marker)


def _one_record(lines: Sequence[str], marker: str, context: str,
                errors: list[str]) -> dict[str, str] | None:
    records = _records(lines, marker)
    if len(records) != 1:
        errors.append(f"expected one {context}, found {len(records)}")
        return records[0] if records else None
    return records[0]


def _integer(record: dict[str, str], name: str, context: str,
             errors: list[str]) -> int | None:
    if name not in record:
        errors.append(f"{context}: missing {name}")
        return None
    try:
        return log_utils.integer(record[name])
    except ValueError:
        errors.append(f"{context}: invalid integer {name}={record[name]!r}")
        return None


def _profile_config(profile: ram_profiles.RamProfile) -> dict[str, object]:
    return {
        "lv_os": "none" if profile.os_none else "freertos",
        "draw_units": profile.draw_units,
        "draw_stack": profile.draw_stack_size or 0,
        "draw_prio": 0 if profile.os_none else 3,
        "freetype_pool": 4096 if profile.small_freetype_pool else 16384,
        "adapter_stack": 32768 if profile.os_none else 8192,
        "lvgl_core": 1 if profile.task_affinity else -1,
        "project_core": 0 if profile.task_affinity else -1,
    }


def _validate_config(record: dict[str, str] | None,
                     profile: ram_profiles.RamProfile,
                     errors: list[str]) -> None:
    if record is None:
        return
    expected = dict(FIXED_CONFIG)
    profile_expected = _profile_config(profile)
    expected.update({
        name: value for name, value in profile_expected.items()
        if name != "lv_os"
    })
    for name, wanted in expected.items():
        actual = _integer(record, name, "config", errors)
        if actual is not None and actual != wanted:
            errors.append(f"config: {name}={actual}, expected {wanted}")
    expected_strings = dict(FIXED_CONFIG_STRINGS)
    expected_strings["lv_os"] = profile_expected["lv_os"]
    for name, wanted in expected_strings.items():
        actual = record.get(name)
        if actual is None:
            errors.append(f"config: missing {name}")
        elif actual != wanted:
            errors.append(f"config: {name}={actual}, expected {wanted}")


def _validate_effects(lines: Sequence[str], errors: list[str]) -> None:
    by_key: dict[tuple[str, str], list[dict[str, str]]] = {}
    for record in _records(lines, "display_bench: display perf "):
        key = (record.get("load", ""), record.get("effect", ""))
        by_key.setdefault(key, []).append(record)
    for load in log_utils.LOADS:
        for effect in log_utils.EFFECTS:
            records = by_key.get((load, effect), [])
            context = f"effect {load}/{effect}"
            if len(records) != 1:
                errors.append(f"expected one {context}, found {len(records)}")
                continue
            record = records[0]
            if record.get("snapshot") != "enabled":
                errors.append(
                    f"{context}: snapshot={record.get('snapshot', '<missing>')}"
                )


def _validate_profile_records(lines: Sequence[str], report: RamRunReport) -> None:
    by_load: dict[str, list[dict[str, str]]] = {}
    for record in _records(lines, "display_bench: display profile "):
        by_load.setdefault(record.get("load", ""), []).append(record)
    for load in log_utils.LOADS:
        records = by_load.get(load, [])
        context = f"profile {load}"
        if len(records) != 1:
            report.validation_errors.append(
                f"expected one {context}, found {len(records)}"
            )
            continue
        record = records[0]
        if record.get("diagnostics") != "PASS":
            report.gate_failures.append(
                f"{context}: diagnostics={record.get('diagnostics', '<missing>')}"
            )
        if record.get("snapshot") != "enabled":
            report.gate_failures.append(
                f"{context}: snapshot={record.get('snapshot', '<missing>')}"
            )
        snapshot_fallbacks = _integer(
            record, "snapshot_fallbacks", context, report.validation_errors
        )
        snapshot_p95 = _integer(
            record, "snapshot_prepare_p95_us", context,
            report.validation_errors,
        )
        minimum_dma = _integer(
            record, "min_dma", context, report.validation_errors
        )
        if snapshot_fallbacks not in (None, 0):
            report.gate_failures.append(
                f"{context}: snapshot_fallbacks={snapshot_fallbacks}"
            )
        if snapshot_p95 is not None and (
                snapshot_p95 == 0 or
                snapshot_p95 > SNAPSHOT_PREPARE_P95_MAXIMUM_US):
            report.gate_failures.append(
                f"{context}: snapshot_prepare_p95_us={snapshot_p95}"
            )
        if minimum_dma is not None and minimum_dma < DMA_LARGEST_MINIMUM:
            report.gate_failures.append(
                f"{context}: min_dma={minimum_dma} < {DMA_LARGEST_MINIMUM}"
            )


def _validate_memory(lines: Sequence[str], profile: ram_profiles.RamProfile,
                     report: RamRunReport) -> None:
    raw_records = _records(lines, "display_bench: display memory ")
    by_load: dict[str, list[dict[str, str]]] = {}
    for record in raw_records:
        if "load" in record:
            by_load.setdefault(record["load"], []).append(record)
    parsed: dict[str, dict[str, int]] = {}
    task_expected = profile.name != "B0"
    psram_expected = profile.name == "C"
    core_expected = 1 if profile.task_affinity else -1
    for load in log_utils.LOADS:
        records = by_load.get(load, [])
        context = f"memory {load}"
        if len(records) != 1:
            report.validation_errors.append(
                f"expected one {context}, found {len(records)}"
            )
            continue
        values: dict[str, int] = {}
        for name in MEMORY_FIELDS:
            value = _integer(
                records[0], name, context, report.validation_errors
            )
            if value is not None:
                values[name] = value
        if len(values) != len(MEMORY_FIELDS):
            continue
        parsed[load] = values
        for name in MEMORY_FIELDS[:6]:
            if values[name] <= 0:
                report.gate_failures.append(
                    f"{context}: {name}={values[name]}"
                )
        if values["render_task"] != int(task_expected):
            report.gate_failures.append(
                f"{context}: render_task={values['render_task']}, "
                f"expected {int(task_expected)}"
            )
        if values["render_stack_psram"] != int(psram_expected):
            report.gate_failures.append(
                f"{context}: render_stack_psram="
                f"{values['render_stack_psram']}, expected "
                f"{int(psram_expected)}"
            )
        high_water = values["render_stack_hwm"]
        if task_expected and high_water < RENDER_STACK_HIGH_WATER_MINIMUM:
            report.gate_failures.append(
                f"{context}: render_stack_hwm={high_water} < "
                f"{RENDER_STACK_HIGH_WATER_MINIMUM}"
            )
        if not task_expected and high_water != 0:
            report.gate_failures.append(
                f"{context}: render_stack_hwm={high_water}, expected 0"
            )
        if values["render_core"] != core_expected:
            report.gate_failures.append(
                f"{context}: render_core={values['render_core']}, "
                f"expected {core_expected}"
            )

    summary = _one_record(
        lines, "display_bench: display memory summary ",
        "memory summary", report.validation_errors,
    )
    if summary is None:
        return
    summary_values: dict[str, int] = {}
    for name in MEMORY_FIELDS:
        value = _integer(
            summary, name, "memory summary", report.validation_errors
        )
        if value is not None:
            summary_values[name] = value
    if len(summary_values) != len(MEMORY_FIELDS):
        return
    if len(parsed) == len(log_utils.LOADS):
        for name in MEMORY_FIELDS[:6] + ("render_stack_hwm",):
            expected = min(values[name] for values in parsed.values())
            if summary_values[name] != expected:
                report.validation_errors.append(
                    f"memory summary {name}={summary_values[name]}, "
                    f"profile minimum={expected}"
                )
    report.minimum_internal_free = summary_values["min_internal_free"]
    report.minimum_internal_largest = summary_values[
        "min_internal_largest"
    ]
    report.minimum_dma_largest = summary_values["min_dma_largest"]
    report.minimum_render_stack_high_water = summary_values[
        "render_stack_hwm"
    ]
    report.render_task_found = bool(summary_values["render_task"])
    report.render_task_stack_in_psram = bool(
        summary_values["render_stack_psram"]
    )
    report.render_task_core_id = summary_values["render_core"]
    if summary_values["render_task"] != int(task_expected):
        report.gate_failures.append(
            f"memory summary: render_task={summary_values['render_task']}, "
            f"expected {int(task_expected)}"
        )
    if summary_values["render_stack_psram"] != int(psram_expected):
        report.gate_failures.append(
            "memory summary: render_stack_psram="
            f"{summary_values['render_stack_psram']}, expected "
            f"{int(psram_expected)}"
        )
    summary_high_water = summary_values["render_stack_hwm"]
    if task_expected and \
            summary_high_water < RENDER_STACK_HIGH_WATER_MINIMUM:
        report.gate_failures.append(
            f"memory summary: render_stack_hwm={summary_high_water} < "
            f"{RENDER_STACK_HIGH_WATER_MINIMUM}"
        )
    if not task_expected and summary_high_water != 0:
        report.gate_failures.append(
            f"memory summary: render_stack_hwm={summary_high_water}, "
            "expected 0"
        )
    if summary_values["render_core"] != core_expected:
        report.gate_failures.append(
            "memory summary: render_core="
            f"{summary_values['render_core']}, expected {core_expected}"
        )


def _validate_final(lines: Sequence[str], report: RamRunReport) -> None:
    record = _one_record(
        lines, "display_bench: display benchmark ",
        "final benchmark", report.validation_errors,
    )
    if record is None:
        return
    stability = record.get("stability")
    performance = record.get("performance")
    state = record.get("state")
    report.performance = performance
    if stability != "PASS":
        report.gate_failures.append(f"benchmark: stability={stability}")
    if performance not in ("TARGET", "FLOOR"):
        report.gate_failures.append(f"benchmark: performance={performance}")
    if state != "COMPLETE":
        report.gate_failures.append(f"benchmark: state={state}")
    if record.get("snapshot") != "enabled":
        report.gate_failures.append(
            f"benchmark: snapshot={record.get('snapshot', '<missing>')}"
        )
    fallbacks = _integer(
        record, "snapshot_fallbacks", "benchmark",
        report.validation_errors,
    )
    snapshot_p95 = _integer(
        record, "snapshot_prepare_max_p95_us", "benchmark",
        report.validation_errors,
    )
    minimum_dma = _integer(
        record, "min_dma", "benchmark", report.validation_errors
    )
    if fallbacks not in (None, 0):
        report.gate_failures.append(
            f"benchmark: snapshot_fallbacks={fallbacks}"
        )
    if snapshot_p95 is not None and (
            snapshot_p95 == 0 or
            snapshot_p95 > SNAPSHOT_PREPARE_P95_MAXIMUM_US):
        report.gate_failures.append(
            f"benchmark: snapshot_prepare_max_p95_us={snapshot_p95}"
        )
    if minimum_dma is not None and minimum_dma < DMA_LARGEST_MINIMUM:
        report.gate_failures.append(
            f"benchmark: min_dma={minimum_dma} < {DMA_LARGEST_MINIMUM}"
        )
    load_record = _one_record(
        lines, "display_bench: display load ",
        "final load", report.validation_errors,
    )
    if load_record is None:
        return
    if load_record.get("profile") != "full":
        report.validation_errors.append(
            "load: profile="
            f"{load_record.get('profile', '<missing>')}, expected full"
        )
    for name, wanted in (
            ("tcp_rate_ok", 1), ("tcp_reconnects", 0),
            ("wifi_disconnects", 0), ("workload", 0), ("control", 0),
            ("audio", 0), ("tcp", 0)):
        actual = _integer(
            load_record, name, "load", report.validation_errors
        )
        if actual is not None and actual != wanted:
            report.gate_failures.append(
                f"load: {name}={actual}, expected {wanted}"
            )


def parse_log(profile_name: str, path: Path) -> RamRunReport:
    """Parse and gate one characterization log."""
    name = profile_name.upper()
    if name not in ram_profiles.PROFILES:
        raise ValueError(f"unknown RAM profile {profile_name!r}")
    report = RamRunReport(name, path)
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        report.validation_errors.append(f"cannot read log: {error}")
        return report
    lines = text.splitlines()
    config = _one_record(
        lines, "display_bench: display config ",
        "display config", report.validation_errors,
    )
    _validate_config(config, ram_profiles.PROFILES[name],
                     report.validation_errors)
    _validate_effects(lines, report.validation_errors)
    _validate_profile_records(lines, report)
    _validate_memory(lines, ram_profiles.PROFILES[name], report)
    _validate_final(lines, report)
    lower_text = text.lower()
    for marker in FATAL_MARKERS:
        if marker in lower_text:
            report.gate_failures.append(f"fatal log marker: {marker}")
    for marker in log_utils.RESTART_MARKERS:
        if text.count(marker) > 1:
            report.gate_failures.append(
                f"repeated startup marker: {marker}"
            )
    report.validation_errors = list(dict.fromkeys(report.validation_errors))
    report.gate_failures = list(dict.fromkeys(report.gate_failures))
    return report


def _apply_gain_gate(baseline: RamRunReport,
                     candidate: RamRunReport) -> None:
    if baseline.minimum_internal_free is None or \
            candidate.minimum_internal_free is None:
        return
    candidate.internal_gain = (
        candidate.minimum_internal_free - baseline.minimum_internal_free
    )
    if candidate.profile == "C":
        required = C_INTERNAL_GAIN_MINIMUM
    else:
        theoretical = ram_profiles.PROFILES[
            candidate.profile
        ].theoretical_reclaim
        required = (theoretical * FALLBACK_RECLAIM_PERCENT + 99) // 100
    if candidate.internal_gain < required:
        candidate.gate_failures.append(
            f"internal gain={candidate.internal_gain} < required {required}"
        )


def analyze(entries: Sequence[tuple[str, Path]]) -> RamAnalysis:
    """Evaluate logs in the fixed B0 -> C -> fallback order."""
    reports: dict[str, RamRunReport] = {}
    reasons: list[str] = []
    for name, path in entries:
        upper_name = name.upper()
        if upper_name in reports:
            reasons.append(f"duplicate log for {upper_name}")
            continue
        reports[upper_name] = parse_log(upper_name, path)
    if reasons:
        return RamAnalysis(reports, None, reasons)
    baseline = reports.get("B0")
    if baseline is None:
        reasons.append("missing B0 baseline log")
        return RamAnalysis(reports, None, reasons)
    for name in ram_profiles.PROFILE_ORDER[1:]:
        if name in reports:
            _apply_gain_gate(baseline, reports[name])
    if baseline.status == "INVALID":
        reasons.append("B0 baseline log is invalid")
        return RamAnalysis(reports, None, reasons)

    synchronous = reports.get("C")
    if synchronous is None:
        reasons.append("missing C log; fallback evaluation cannot start")
        return RamAnalysis(reports, None, reasons)
    if synchronous.status == "INVALID":
        reasons.append("C log is invalid; fallback evaluation cannot start")
        return RamAnalysis(reports, None, reasons)
    if synchronous.status == "PASS":
        return RamAnalysis(reports, "C", reasons)

    passed_fallbacks: list[RamRunReport] = []
    fallback_sequence_complete = False
    for name in ram_profiles.FALLBACK_PROFILE_ORDER:
        report = reports.get(name)
        if report is None:
            reasons.append(f"missing {name} log; stopped fallback sequence")
            break
        if report.status == "INVALID":
            reasons.append(f"{name} log is invalid; stopped fallback sequence")
            break
        if report.status == "FAIL":
            reasons.append(f"{name} is {report.status}; stopped smaller stacks")
            fallback_sequence_complete = True
            break
        passed_fallbacks.append(report)
    else:
        fallback_sequence_complete = True
    if not fallback_sequence_complete or not passed_fallbacks:
        return RamAnalysis(reports, None, reasons)
    selected = max(
        passed_fallbacks,
        key=lambda item: (
            item.internal_gain if item.internal_gain is not None else -1,
            item.performance == "TARGET",
        ),
    )
    return RamAnalysis(reports, selected.profile, reasons)


def _parse_log_argument(value: str) -> tuple[str, Path]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("expected PROFILE=PATH")
    name, raw_path = value.split("=", 1)
    name = name.upper()
    if name not in ram_profiles.PROFILES:
        raise argparse.ArgumentTypeError(f"unknown RAM profile {name!r}")
    if not raw_path:
        raise argparse.ArgumentTypeError("log path is empty")
    return name, Path(raw_path)


def _json_report(analysis: RamAnalysis) -> dict[str, object]:
    reports = {}
    for name, report in analysis.reports.items():
        values = asdict(report)
        values["path"] = str(report.path)
        values["status"] = report.status
        reports[name] = values
    return {
        "selected": analysis.selected,
        "reasons": analysis.reasons,
        "reports": reports,
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Gate LVGL internal-RAM characterization logs"
    )
    parser.add_argument(
        "--log", action="append", type=_parse_log_argument, required=True,
        metavar="PROFILE=PATH",
    )
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    analysis = analyze(args.log)
    if args.json:
        print(json.dumps(_json_report(analysis), indent=2, sort_keys=True))
    else:
        for name in ram_profiles.PROFILE_ORDER:
            report = analysis.reports.get(name)
            if report is None:
                continue
            gain = "n/a" if report.internal_gain is None else \
                str(report.internal_gain)
            print(
                f"{name}: {report.status} min_internal_free="
                f"{report.minimum_internal_free} gain={gain}"
            )
            for error in report.validation_errors + report.gate_failures:
                print(f"  - {error}")
        for reason in analysis.reasons:
            print(f"sequence: {reason}")
        print(f"selected: {analysis.selected or 'NONE'}")
    if any(report.validation_errors for report in analysis.reports.values()):
        return 2
    return 0 if analysis.selected is not None else 1


if __name__ == "__main__":
    raise SystemExit(main())
