#!/usr/bin/env python3
"""Analyze matched 40/80 MHz display characterization logs."""

from __future__ import annotations

import argparse
import json
import math
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Sequence


DMA_LARGEST_MINIMUM = 14_720
EXPECTED_PROFILE_SAMPLES = 150
EXPECTED_TOTAL_SAMPLES = 300
P95_REQUIRED_PERCENT = 95
FPS_MINIMUM_PERCENT = 95
PANEL_COST_REQUIRED_PERCENT = 65

EFFECTS = (
    "fade",
    "push-left",
    "push-right",
    "cover-left",
    "reveal-right",
)
LOADS = ("display-only", "full")


@dataclass(frozen=True)
class ProfileSpec:
    family: str
    clock_hz: int
    draw_rows: int
    color: str


PROFILE_SPECS = {
    "E40": ProfileSpec("E", 40_000_000, 120, "RGB565_SWAPPED"),
    "E80": ProfileSpec("E", 80_000_000, 120, "RGB565_SWAPPED"),
    "P40": ProfileSpec("P", 40_000_000, 60, "RGB565"),
    "P80": ProfileSpec("P", 80_000_000, 60, "RGB565"),
}

PAIR_LABELS = (("E40", "E80"), ("P40", "P80"))
EXPECTED_RUN_COUNTS = {"E40": 2, "E80": 2, "P40": 1, "P80": 1}

FINGERPRINT_EXPECTED = {
    "dma_rows": 10,
    "dma_max_full_rows": 44,
    "queue": 2,
    "direct": 0,
    "te": 0,
    "draw_units": 2,
    "draw_prio": 3,
    "tcp_payload": 5760,
    "tcp_prio": 2,
    "load_profile": "full",
    "lifecycle_log": 0,
}

PERF_INTEGER_FIELDS = (
    "start",
    "complete",
    "cancel",
    "active_frames",
    "active_ms",
    "avg_fps_x100",
    "intervals",
    "target_pct_x100",
    "p50_us",
    "p95_us",
    "p99_us",
    "max_us",
    "long_run",
)

COST_INTEGER_FIELDS = (
    "render_count",
    "render_us",
    "render_max_us",
    "flush_count",
    "flush_pixels",
    "flush_us",
    "flush_max_us",
    "flush_wait_count",
    "flush_wait_us",
    "flush_wait_max_us",
    "panel_count",
    "panel_pixels",
    "panel_us",
    "panel_max_us",
)

PROFILE_INTEGER_FIELDS = (
    "samples",
    "sample_err",
    "lock_err",
    "fps_read_err",
    "fps_lock_max_us",
    "min_fps",
    "fps_below_30",
    "min_dma",
    "dma_fail",
    "frame_submits",
    "panel_submits",
    "submit_fail",
)

BENCHMARK_INTEGER_FIELDS = (
    "samples",
    "sample_err",
    "lock_err",
    "fps_read_err",
    "fps_lock_max_us",
    "min_dma",
    "dma_fail",
    "frame_submits",
    "submit_fail",
    "transition_cancel",
)

LOAD_INTEGER_FIELDS = (
    "tcp_required",
    "tcp_tx_bytes",
    "tcp_rx_bytes",
    "tcp_target_bytes",
    "tcp_active_us",
    "tcp_rate_ok",
    "tcp_reconnects",
    "tcp_down_ms",
    "tcp_pacing_late",
    "tcp_pacing_max_lag_us",
    "wifi_disconnects",
    "workload",
    "control",
    "audio",
    "tcp",
)

FATAL_MARKERS = (
    "guru meditation error",
    "task watchdog got triggered",
    "interrupt wdt timeout",
    "watchdog timeout",
    "stack overflow",
    "stack smashing protect failure",
    "corrupt heap",
    "heap corruption",
    "assert failed:",
    "abort() was called",
    "brownout detector was triggered",
    "esp_err_no_mem",
    "panic'ed",
)

RESTART_MARKERS = (
    "boot: ESP-IDF",
    "rst:0x",
)


@dataclass
class EffectMetrics:
    load: str
    effect: str
    average_fps_x100: int
    p95_us: int
    active_frames: int
    panel_us: int

    @property
    def panel_frame_us(self) -> float:
        return self.panel_us / self.active_frames


@dataclass
class RunReport:
    label: str
    path: Path
    effects: dict[tuple[str, str], EffectMetrics] = field(default_factory=dict)
    minimum_dma: int | None = None
    dma_failure_count: int = 0
    validation_errors: list[str] = field(default_factory=list)
    transport_errors: list[str] = field(default_factory=list)
    stability_errors: list[str] = field(default_factory=list)

    @property
    def valid(self) -> bool:
        return not self.validation_errors

    @property
    def transport_passed(self) -> bool:
        return self.valid and not self.transport_errors

    @property
    def system_stability_passed(self) -> bool:
        return self.transport_passed and not self.stability_errors


@dataclass
class AggregateReport:
    label: str
    runs: list[RunReport]
    effects: dict[tuple[str, str], dict[str, float]]
    worst_average_p95_us: float | None
    panel_frame_us: float | None
    minimum_dma: int | None

    @property
    def valid(self) -> bool:
        return bool(self.runs) and all(run.valid for run in self.runs)

    @property
    def transport_passed(self) -> bool:
        return self.valid and all(run.transport_passed for run in self.runs)

    @property
    def system_stability_passed(self) -> bool:
        return self.transport_passed and all(
            run.system_stability_passed for run in self.runs
        )


@dataclass
class ComparisonReport:
    family: str
    baseline: str
    candidate: str
    status: str
    transport_passed: bool
    system_stability_passed: bool
    p95_improvement_percent: float | None
    panel_reduction_percent: float | None
    fps_regressions: list[dict[str, float]]
    reasons: list[str]


def _parse_key_values(fragment: str) -> dict[str, str]:
    fields = {}
    for token in fragment.strip().split():
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        fields[key] = value.rstrip("\r")
    return fields


def _integer(value: str) -> int:
    if value.lower().startswith(("0x", "+0x", "-0x")):
        return int(value, 16)
    return int(value, 10)


def _required_integer_fields(
    raw: dict[str, str],
    names: Iterable[str],
    context: str,
    errors: list[str],
) -> dict[str, int]:
    values = {}
    for name in names:
        if name not in raw:
            errors.append(f"{context}: missing {name}")
            continue
        try:
            values[name] = _integer(raw[name])
        except ValueError:
            errors.append(f"{context}: invalid integer {name}={raw[name]!r}")
    return values


def _records(lines: Sequence[str], marker: str) -> list[dict[str, str]]:
    records = []
    for line in lines:
        offset = line.find(marker)
        if offset >= 0:
            records.append(_parse_key_values(line[offset + len(marker) :]))
    return records


def _colors(lines: Sequence[str]) -> list[str]:
    marker = "app_adapter: display color format: "
    values = []
    for line in lines:
        offset = line.find(marker)
        if offset >= 0:
            tail = line[offset + len(marker) :].strip()
            if tail:
                values.append(tail.split()[0].rstrip("\r"))
    return values


def _consistent_record(
    records: list[dict[str, str]], context: str, errors: list[str]
) -> dict[str, str] | None:
    if not records:
        errors.append(f"missing {context}")
        return None
    if len(records) != 1:
        errors.append(f"expected one {context} record, found {len(records)}")
    return records[0]


def _consistent_value(
    values: list[str], context: str, errors: list[str]
) -> str | None:
    if not values:
        errors.append(f"missing {context}")
        return None
    if len(values) != 1:
        errors.append(f"expected one {context}, found {len(values)}")
    return values[0]


def _one_record(
    records: list[dict[str, str]], context: str, errors: list[str]
) -> dict[str, str] | None:
    if len(records) != 1:
        errors.append(f"expected one {context} record, found {len(records)}")
        return None
    return records[0]


def _validate_fingerprint(
    spec: ProfileSpec,
    board: dict[str, str] | None,
    color: str | None,
    config: dict[str, str] | None,
    errors: list[str],
) -> None:
    if board is not None:
        values = _required_integer_fields(
            board,
            ("clock_hz", "max_lines", "queue", "direct", "te"),
            "board fingerprint",
            errors,
        )
        expected = {
            "clock_hz": spec.clock_hz,
            "max_lines": 10,
            "queue": 2,
            "direct": 0,
            "te": 0,
        }
        for key, expected_value in expected.items():
            if key in values and values[key] != expected_value:
                errors.append(
                    f"board fingerprint: {key}={values[key]}, expected {expected_value}"
                )

    if color is not None and color != spec.color:
        errors.append(f"adapter fingerprint: color={color}, expected {spec.color}")

    if config is None:
        return
    numeric_names = (
        "qspi_hz",
        "draw_rows",
        "dma_rows",
        "dma_max_full_rows",
        "queue",
        "direct",
        "te",
        "draw_units",
        "draw_prio",
        "tcp_payload",
        "tcp_prio",
        "lifecycle_log",
    )
    values = _required_integer_fields(config, numeric_names, "config", errors)
    expected_config = {
        "qspi_hz": spec.clock_hz,
        "draw_rows": spec.draw_rows,
        **{
            key: value
            for key, value in FINGERPRINT_EXPECTED.items()
            if isinstance(value, int)
        },
    }
    for key, expected_value in expected_config.items():
        if key in values and values[key] != expected_value:
            errors.append(f"config: {key}={values[key]}, expected {expected_value}")
    for key in ("color", "load_profile"):
        expected_value = spec.color if key == "color" else FINGERPRINT_EXPECTED[key]
        if key not in config:
            errors.append(f"config: missing {key}")
        elif config[key] != expected_value:
            errors.append(f"config: {key}={config[key]}, expected {expected_value}")


def _validate_effect_records(
    perf_records: list[dict[str, str]],
    cost_records: list[dict[str, str]],
    run: RunReport,
) -> None:
    perf_by_key: dict[tuple[str, str], list[dict[str, str]]] = defaultdict(list)
    cost_by_key: dict[tuple[str, str], list[dict[str, str]]] = defaultdict(list)
    for context, records, destination in (
        ("perf", perf_records, perf_by_key),
        ("cost", cost_records, cost_by_key),
    ):
        for record in records:
            load = record.get("load")
            effect = record.get("effect")
            if load is None or effect is None:
                run.validation_errors.append(
                    f"{context} record missing load or effect"
                )
                continue
            destination[(load, effect)].append(record)

    expected_keys = {(load, effect) for load in LOADS for effect in EFFECTS}
    for kind, records in (("perf", perf_by_key), ("cost", cost_by_key)):
        unexpected = sorted(set(records) - expected_keys)
        if unexpected:
            run.validation_errors.append(f"unexpected {kind} records: {unexpected}")
        for key in sorted(expected_keys):
            count = len(records.get(key, []))
            if count != 1:
                run.validation_errors.append(
                    f"expected one {kind} record for {key[0]}/{key[1]}, found {count}"
                )

    for key in sorted(expected_keys):
        if len(perf_by_key.get(key, [])) != 1 or len(cost_by_key.get(key, [])) != 1:
            continue
        perf_raw = perf_by_key[key][0]
        cost_raw = cost_by_key[key][0]
        context = f"{key[0]}/{key[1]}"
        if "result" not in perf_raw:
            run.validation_errors.append(f"{context} perf: missing result")
        perf = _required_integer_fields(
            perf_raw, PERF_INTEGER_FIELDS, f"{context} perf", run.validation_errors
        )
        cost = _required_integer_fields(
            cost_raw, COST_INTEGER_FIELDS, f"{context} cost", run.validation_errors
        )
        if not all(name in perf for name in PERF_INTEGER_FIELDS) or not all(
            name in cost for name in COST_INTEGER_FIELDS
        ):
            continue
        if perf["start"] <= 0:
            run.transport_errors.append(f"{context}: no completed workload starts")
        if perf["complete"] != perf["start"]:
            run.transport_errors.append(
                f"{context}: start={perf['start']} complete={perf['complete']}"
            )
        if perf["cancel"] != 0:
            run.transport_errors.append(f"{context}: cancel={perf['cancel']}")
        if perf["active_frames"] <= 0 or perf["intervals"] <= 0:
            run.transport_errors.append(f"{context}: missing active frame intervals")
        if perf["avg_fps_x100"] <= 0 or perf["p95_us"] <= 0:
            run.transport_errors.append(f"{context}: invalid FPS or P95 result")
        if cost["panel_count"] <= 0 or cost["panel_us"] <= 0:
            run.transport_errors.append(f"{context}: missing panel submissions")
        if perf["active_frames"] <= 0:
            continue
        run.effects[key] = EffectMetrics(
            load=key[0],
            effect=key[1],
            average_fps_x100=perf["avg_fps_x100"],
            p95_us=perf["p95_us"],
            active_frames=perf["active_frames"],
            panel_us=cost["panel_us"],
        )


def _validate_profiles(
    records: list[dict[str, str]], run: RunReport
) -> dict[str, dict[str, int]]:
    by_load: dict[str, list[dict[str, str]]] = defaultdict(list)
    for record in records:
        load = record.get("load")
        if load is None:
            run.validation_errors.append("profile record missing load")
            continue
        by_load[load].append(record)
    unexpected = sorted(set(by_load) - set(LOADS))
    if unexpected:
        run.validation_errors.append(f"unexpected profile records: {unexpected}")

    parsed = {}
    for load in LOADS:
        candidates = by_load.get(load, [])
        if len(candidates) != 1:
            run.validation_errors.append(
                f"expected one profile record for {load}, found {len(candidates)}"
            )
            continue
        raw = candidates[0]
        if "diagnostics" not in raw:
            run.validation_errors.append(f"profile {load}: missing diagnostics")
        values = _required_integer_fields(
            raw,
            PROFILE_INTEGER_FIELDS,
            f"profile {load}",
            run.validation_errors,
        )
        if not all(name in values for name in PROFILE_INTEGER_FIELDS):
            continue
        parsed[load] = values
        if values["samples"] != EXPECTED_PROFILE_SAMPLES:
            run.transport_errors.append(
                f"profile {load}: samples={values['samples']}, "
                f"expected {EXPECTED_PROFILE_SAMPLES}"
            )
        for name in ("sample_err", "lock_err", "fps_read_err", "submit_fail"):
            if values[name] != 0:
                run.transport_errors.append(
                    f"profile {load}: {name}={values[name]}"
                )
        if values["frame_submits"] <= 0 or values["panel_submits"] <= 0:
            run.transport_errors.append(f"profile {load}: no display submissions")
        if values["min_dma"] < DMA_LARGEST_MINIMUM:
            run.stability_errors.append(
                f"profile {load}: min_dma={values['min_dma']} < {DMA_LARGEST_MINIMUM}"
            )
        if values["dma_fail"] != 0:
            run.stability_errors.append(
                f"profile {load}: dma_fail={values['dma_fail']}"
            )
    return parsed


def _validate_final_records(
    benchmark_raw: dict[str, str] | None,
    load_raw: dict[str, str] | None,
    profiles: dict[str, dict[str, int]],
    run: RunReport,
) -> None:
    if benchmark_raw is None or load_raw is None:
        return
    for name in ("stability", "performance", "state"):
        if name not in benchmark_raw:
            run.validation_errors.append(f"benchmark: missing {name}")
    benchmark = _required_integer_fields(
        benchmark_raw,
        BENCHMARK_INTEGER_FIELDS,
        "benchmark",
        run.validation_errors,
    )
    if "profile" not in load_raw:
        run.validation_errors.append("load: missing profile")
    load = _required_integer_fields(
        load_raw, LOAD_INTEGER_FIELDS, "load", run.validation_errors
    )
    if not all(name in benchmark for name in BENCHMARK_INTEGER_FIELDS) or not all(
        name in load for name in LOAD_INTEGER_FIELDS
    ):
        return

    if benchmark_raw.get("state") != "COMPLETE":
        run.transport_errors.append(
            f"benchmark state={benchmark_raw.get('state', 'missing')}"
        )
    for name in ("sample_err", "lock_err", "fps_read_err", "submit_fail", "transition_cancel"):
        if benchmark[name] != 0:
            run.transport_errors.append(f"benchmark {name}={benchmark[name]}")
    if benchmark["samples"] != EXPECTED_TOTAL_SAMPLES:
        run.transport_errors.append(
            f"benchmark samples={benchmark['samples']}, "
            f"expected {EXPECTED_TOTAL_SAMPLES}"
        )
    if benchmark["frame_submits"] <= 0:
        run.transport_errors.append("benchmark has no frame submissions")

    if load_raw.get("profile") != "full":
        run.validation_errors.append(
            f"load: profile={load_raw.get('profile')}, expected full"
        )
    if load["tcp_required"] != 1:
        run.validation_errors.append(
            f"load: tcp_required={load['tcp_required']}, expected 1"
        )
    if load["tcp_rate_ok"] != 1:
        run.transport_errors.append(f"load: tcp_rate_ok={load['tcp_rate_ok']}")
    if load["tcp_reconnects"] != 0:
        run.transport_errors.append(
            f"load: tcp_reconnects={load['tcp_reconnects']}"
        )
    if load["wifi_disconnects"] != 0:
        run.transport_errors.append(
            f"load: wifi_disconnects={load['wifi_disconnects']}"
        )
    for name in ("workload", "control", "audio", "tcp"):
        if load[name] != 0:
            run.transport_errors.append(f"load: {name}=0x{load[name]:x}")
    minimum_tcp_bytes = math.ceil(load["tcp_target_bytes"] * 0.95)
    if load["tcp_active_us"] <= 0 or load["tcp_target_bytes"] <= 0:
        run.transport_errors.append("load: TCP did not run")
    if load["tcp_tx_bytes"] < minimum_tcp_bytes:
        run.transport_errors.append(
            f"load: tcp_tx_bytes={load['tcp_tx_bytes']} < {minimum_tcp_bytes}"
        )
    if load["tcp_rx_bytes"] < minimum_tcp_bytes:
        run.transport_errors.append(
            f"load: tcp_rx_bytes={load['tcp_rx_bytes']} < {minimum_tcp_bytes}"
        )

    run.minimum_dma = benchmark["min_dma"]
    run.dma_failure_count = benchmark["dma_fail"]
    if benchmark["min_dma"] < DMA_LARGEST_MINIMUM:
        run.stability_errors.append(
            f"benchmark min_dma={benchmark['min_dma']} < {DMA_LARGEST_MINIMUM}"
        )
    if benchmark["dma_fail"] != 0:
        run.stability_errors.append(f"benchmark dma_fail={benchmark['dma_fail']}")

    if len(profiles) == len(LOADS):
        sums = {
            "samples": sum(item["samples"] for item in profiles.values()),
            "sample_err": sum(item["sample_err"] for item in profiles.values()),
            "lock_err": sum(item["lock_err"] for item in profiles.values()),
            "fps_read_err": sum(
                item["fps_read_err"] for item in profiles.values()
            ),
            "dma_fail": sum(item["dma_fail"] for item in profiles.values()),
            "frame_submits": sum(
                item["frame_submits"] for item in profiles.values()
            ),
            "submit_fail": sum(item["submit_fail"] for item in profiles.values()),
        }
        for name, expected in sums.items():
            if benchmark[name] != expected:
                run.validation_errors.append(
                    f"benchmark {name}={benchmark[name]}, profile sum={expected}"
                )
        expected_min_dma = min(item["min_dma"] for item in profiles.values())
        if benchmark["min_dma"] != expected_min_dma:
            run.validation_errors.append(
                f"benchmark min_dma={benchmark['min_dma']}, "
                f"profile minimum={expected_min_dma}"
            )

    reported_stability = benchmark_raw.get("stability")
    if reported_stability == "PASS" and (
        run.transport_errors or run.stability_errors
    ):
        run.validation_errors.append(
            "benchmark reports stability=PASS despite reported failures"
        )
    elif reported_stability == "FAIL" and not (
        run.transport_errors or run.stability_errors
    ):
        run.transport_errors.append(
            "benchmark reports stability=FAIL without a reported cause"
        )
    elif reported_stability not in ("PASS", "FAIL"):
        run.validation_errors.append(
            f"benchmark: invalid stability={reported_stability}"
        )


def parse_log(label: str, path: Path) -> RunReport:
    label = label.upper()
    if label not in PROFILE_SPECS:
        raise ValueError(f"unknown profile {label!r}")
    run = RunReport(label=label, path=path)
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        run.validation_errors.append(f"cannot read log: {error}")
        return run
    lines = text.splitlines()

    board = _consistent_record(
        _records(lines, "board_display: LCD SPI "),
        "board fingerprint",
        run.validation_errors,
    )
    color = _consistent_value(
        _colors(lines), "adapter color fingerprint", run.validation_errors
    )
    config = _consistent_record(
        _records(lines, "display_bench: display config "),
        "benchmark config fingerprint",
        run.validation_errors,
    )
    _validate_fingerprint(
        PROFILE_SPECS[label], board, color, config, run.validation_errors
    )

    _validate_effect_records(
        _records(lines, "display_bench: display perf "),
        _records(lines, "display_bench: display cost "),
        run,
    )
    profiles = _validate_profiles(
        _records(lines, "display_bench: display profile "), run
    )
    benchmark = _one_record(
        _records(lines, "display_bench: display benchmark "),
        "final benchmark",
        run.validation_errors,
    )
    load = _one_record(
        _records(lines, "display_bench: display load "),
        "final load",
        run.validation_errors,
    )
    _validate_final_records(benchmark, load, profiles, run)

    lower_text = text.lower()
    for marker in FATAL_MARKERS:
        if marker in lower_text:
            run.transport_errors.append(f"fatal log marker: {marker}")
    for marker in RESTART_MARKERS:
        if text.count(marker) > 1:
            run.transport_errors.append(f"repeated startup marker: {marker}")
    run.validation_errors = list(dict.fromkeys(run.validation_errors))
    run.transport_errors = list(dict.fromkeys(run.transport_errors))
    run.stability_errors = list(dict.fromkeys(run.stability_errors))
    return run


def aggregate_profile(label: str, runs: list[RunReport]) -> AggregateReport:
    effect_values: dict[tuple[str, str], dict[str, float]] = {}
    if runs and all(run.valid and len(run.effects) == len(EFFECTS) * len(LOADS) for run in runs):
        for key in sorted(runs[0].effects):
            metrics = [run.effects[key] for run in runs]
            effect_values[key] = {
                "average_p95_us": math.fsum(item.p95_us for item in metrics)
                / len(metrics),
                "average_fps_x100": math.fsum(
                    item.average_fps_x100 for item in metrics
                )
                / len(metrics),
                "average_panel_frame_us": math.fsum(
                    item.panel_frame_us for item in metrics
                )
                / len(metrics),
            }
    worst_p95 = (
        max(value["average_p95_us"] for value in effect_values.values())
        if effect_values
        else None
    )
    all_effects = (
        [effect for run in runs for effect in run.effects.values()]
        if effect_values
        else []
    )
    total_frames = sum(effect.active_frames for effect in all_effects)
    panel_frame_us = (
        sum(effect.panel_us for effect in all_effects) / total_frames
        if total_frames > 0
        else None
    )
    minima = [run.minimum_dma for run in runs if run.minimum_dma is not None]
    return AggregateReport(
        label=label,
        runs=runs,
        effects=effect_values,
        worst_average_p95_us=worst_p95,
        panel_frame_us=panel_frame_us,
        minimum_dma=min(minima) if minima else None,
    )


def compare_profiles(
    baseline: AggregateReport, candidate: AggregateReport
) -> ComparisonReport:
    family = PROFILE_SPECS[baseline.label].family
    reasons = []
    fps_regressions = []
    p95_improvement = None
    panel_reduction = None

    input_transport_passed = baseline.transport_passed and candidate.transport_passed
    if not baseline.valid or not candidate.valid:
        reasons.append("one or more logs failed structural validation")
    elif not input_transport_passed:
        reasons.append("one or more runs failed transport/runtime validation")
    elif set(baseline.effects) != set(candidate.effects):
        reasons.append("40/80 MHz effect sets differ")
    elif baseline.worst_average_p95_us is None or candidate.worst_average_p95_us is None:
        reasons.append("missing P95 data")
    elif baseline.panel_frame_us is None or candidate.panel_frame_us is None:
        reasons.append("missing panel cost data")
    else:
        for aggregate in (baseline, candidate):
            expected_count = EXPECTED_RUN_COUNTS[aggregate.label]
            if len(aggregate.runs) != expected_count:
                reasons.append(
                    f"{aggregate.label} has {len(aggregate.runs)} runs, "
                    f"expected {expected_count}"
                )
        if not any(run.stability_errors for run in baseline.runs) and any(
                run.stability_errors for run in candidate.runs):
            reasons.append(
                "80 MHz introduced a DMA/internal memory stability failure"
            )
        p95_improvement = 100.0 * (
            1.0
            - candidate.worst_average_p95_us / baseline.worst_average_p95_us
        )
        panel_reduction = 100.0 * (
            1.0 - candidate.panel_frame_us / baseline.panel_frame_us
        )
        if candidate.worst_average_p95_us * 100 > (
            baseline.worst_average_p95_us * P95_REQUIRED_PERCENT
        ):
            reasons.append("worst averaged P95 improved by less than 5%")
        if candidate.panel_frame_us * 100 > (
            baseline.panel_frame_us * PANEL_COST_REQUIRED_PERCENT
        ):
            reasons.append("panel cost per active frame fell by less than 35%")
        for key in sorted(baseline.effects):
            baseline_fps = baseline.effects[key]["average_fps_x100"]
            candidate_fps = candidate.effects[key]["average_fps_x100"]
            if candidate_fps * 100 < baseline_fps * FPS_MINIMUM_PERCENT:
                regression = 100.0 * (1.0 - candidate_fps / baseline_fps)
                fps_regressions.append(
                    {
                        "load": key[0],
                        "effect": key[1],
                        "baseline_fps": baseline_fps / 100.0,
                        "candidate_fps": candidate_fps / 100.0,
                        "regression_percent": regression,
                    }
                )
        if fps_regressions:
            reasons.append("one or more effects regressed in average FPS by over 5%")

    comparable = (
        baseline.valid
        and candidate.valid
        and input_transport_passed
        and p95_improvement is not None
        and panel_reduction is not None
    )
    transport_passed = comparable and not reasons
    return ComparisonReport(
        family=family,
        baseline=baseline.label,
        candidate=candidate.label,
        status="PASS" if transport_passed else ("FAIL" if comparable else "INVALID"),
        transport_passed=transport_passed,
        system_stability_passed=(
            baseline.system_stability_passed
            and candidate.system_stability_passed
        ),
        p95_improvement_percent=p95_improvement,
        panel_reduction_percent=panel_reduction,
        fps_regressions=fps_regressions,
        reasons=reasons,
    )


def analyze(
    entries: Sequence[tuple[str, Path]],
) -> tuple[dict[str, AggregateReport], list[ComparisonReport]]:
    grouped: dict[str, list[RunReport]] = defaultdict(list)
    seen_paths: dict[Path, RunReport] = {}
    for label, path in entries:
        normalized = label.upper()
        run = parse_log(normalized, path)
        canonical_path = path.resolve()
        previous = seen_paths.get(canonical_path)
        if previous is not None:
            error = f"duplicate input log path: {canonical_path}"
            previous.validation_errors.append(error)
            run.validation_errors.append(error)
        else:
            seen_paths[canonical_path] = run
        grouped[normalized].append(run)
    aggregates = {
        label: aggregate_profile(label, runs) for label, runs in grouped.items()
    }
    comparisons = []
    for baseline, candidate in PAIR_LABELS:
        if baseline in aggregates and candidate in aggregates:
            comparisons.append(compare_profiles(aggregates[baseline], aggregates[candidate]))
    return aggregates, comparisons


def _run_to_json(run: RunReport) -> dict[str, object]:
    return {
        "path": str(run.path),
        "valid": run.valid,
        "transport_passed": run.transport_passed,
        "system_stability_passed": run.system_stability_passed,
        "minimum_dma": run.minimum_dma,
        "dma_failure_count": run.dma_failure_count,
        "validation_errors": run.validation_errors,
        "transport_errors": run.transport_errors,
        "stability_errors": run.stability_errors,
    }


def result_as_dict(
    aggregates: dict[str, AggregateReport], comparisons: list[ComparisonReport]
) -> dict[str, object]:
    profiles = {}
    for label in sorted(aggregates):
        aggregate = aggregates[label]
        profiles[label] = {
            "run_count": len(aggregate.runs),
            "valid": aggregate.valid,
            "transport_passed": aggregate.transport_passed,
            "system_stability_passed": aggregate.system_stability_passed,
            "minimum_dma": aggregate.minimum_dma,
            "worst_average_p95_us": aggregate.worst_average_p95_us,
            "panel_frame_us": aggregate.panel_frame_us,
            "effects": {
                f"{load}/{effect}": values
                for (load, effect), values in aggregate.effects.items()
            },
            "runs": [_run_to_json(run) for run in aggregate.runs],
        }
    return {
        "profiles": profiles,
        "comparisons": [
            {
                "family": item.family,
                "baseline": item.baseline,
                "candidate": item.candidate,
                "status": item.status,
                "transport_passed": item.transport_passed,
                "system_stability_passed": item.system_stability_passed,
                "p95_improvement_percent": item.p95_improvement_percent,
                "panel_reduction_percent": item.panel_reduction_percent,
                "fps_regressions": item.fps_regressions,
                "reasons": item.reasons,
            }
            for item in comparisons
        ],
    }


def _format_optional(value: float | None, digits: int = 1) -> str:
    return "n/a" if value is None else f"{value:.{digits}f}"


def print_human(
    aggregates: dict[str, AggregateReport], comparisons: list[ComparisonReport]
) -> None:
    for label in sorted(aggregates):
        aggregate = aggregates[label]
        print(
            f"{label}: runs={len(aggregate.runs)} "
            f"valid={'PASS' if aggregate.valid else 'FAIL'} "
            f"runtime={'PASS' if aggregate.transport_passed else 'FAIL'} "
            f"system={'PASS' if aggregate.system_stability_passed else 'FAIL'} "
            f"W_p95_us={_format_optional(aggregate.worst_average_p95_us)} "
            f"panel_frame_us={_format_optional(aggregate.panel_frame_us, 2)} "
            f"min_dma={aggregate.minimum_dma if aggregate.minimum_dma is not None else 'n/a'}"
        )
        for run in aggregate.runs:
            for category, errors in (
                ("validation", run.validation_errors),
                ("runtime", run.transport_errors),
                ("system", run.stability_errors),
            ):
                for error in errors:
                    print(f"  {run.path}: {category}: {error}")

    for comparison in comparisons:
        print(
            f"{comparison.baseline}->{comparison.candidate}: "
            f"transport={comparison.status} "
            f"system={'PASS' if comparison.system_stability_passed else 'FAIL'} "
            f"p95_improvement_pct={_format_optional(comparison.p95_improvement_percent, 2)} "
            f"panel_reduction_pct={_format_optional(comparison.panel_reduction_percent, 2)} "
            f"fps_regressions={len(comparison.fps_regressions)}"
        )
        for reason in comparison.reasons:
            print(f"  reason: {reason}")
        if comparison.transport_passed and not comparison.system_stability_passed:
            print(
                "  note: clock transport passed, but DMA/internal memory stability did not"
            )


def _log_entry(value: str) -> tuple[str, Path]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("expected PROFILE=PATH")
    label, raw_path = value.split("=", 1)
    label = label.upper()
    if label not in PROFILE_SPECS:
        choices = ", ".join(PROFILE_SPECS)
        raise argparse.ArgumentTypeError(
            f"unknown profile {label!r}; choose one of {choices}"
        )
    if not raw_path:
        raise argparse.ArgumentTypeError("log path cannot be empty")
    return label, Path(raw_path)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Validate and compare matched 40/80 MHz display logs."
    )
    parser.add_argument(
        "--log",
        action="append",
        required=True,
        type=_log_entry,
        metavar="PROFILE=PATH",
        help="add an E40, E80, P40, or P80 device log; repeat for duplicate runs",
    )
    parser.add_argument(
        "--json", action="store_true", help="emit machine-readable JSON instead"
    )
    args = parser.parse_args(argv)
    aggregates, comparisons = analyze(args.log)
    complete_pairs = {
        (comparison.baseline, comparison.candidate) for comparison in comparisons
    }
    supplied_labels = set(aggregates)
    unmatched = [
        label
        for label in sorted(supplied_labels)
        if not any(label in pair and pair in complete_pairs for pair in PAIR_LABELS)
    ]
    if args.json:
        output = result_as_dict(aggregates, comparisons)
        output["unmatched_profiles"] = unmatched
        print(json.dumps(output, indent=2, sort_keys=True))
    else:
        print_human(aggregates, comparisons)
        if unmatched:
            print(f"unmatched profiles: {', '.join(unmatched)}")

    inputs_valid = all(aggregate.valid for aggregate in aggregates.values())
    return 0 if comparisons and inputs_valid and all(
        comparison.transport_passed for comparison in comparisons
    ) and not unmatched else 1


if __name__ == "__main__":
    sys.exit(main())
