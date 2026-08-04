#!/usr/bin/env python3
"""Validate one C_EXT_STRESS on-device log against its fixed gates."""

from __future__ import annotations

import argparse
import json
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Sequence

import analyze_clock_ab as clock_analyzer


PHASES = (
    "begin",
    "load_start",
    "provisioning_wait",
    "warmup",
    "measure",
    "cleanup",
    "end",
)
TASKS = {
    "lvgl",
    "connectivity",
    "wifi_service",
    "provisioning",
    "nimble_host",
    "display_bench",
    "display_tcp",
    "stress_audio_tx",
    "stress_audio_rx",
    "stress_sampler",
}
PSRAM_TASKS = {
    "lvgl",
    "display_bench",
    "display_tcp",
    "stress_audio_tx",
    "stress_audio_rx",
    "stress_sampler",
}
DMA_LARGEST_MINIMUM = 14_720
SNAPSHOT_P95_MAXIMUM_US = 100_000
FATAL_MARKERS = clock_analyzer.FATAL_MARKERS + (
    "display submit failed",
    "display frozen",
    "lvgl lock sample failed",
)


@dataclass
class StressAnalysis:
    path: Path
    phases: list[str] = field(default_factory=list)
    validation_errors: list[str] = field(default_factory=list)
    gate_failures: list[str] = field(default_factory=list)

    @property
    def status(self) -> str:
        if self.validation_errors:
            return "INVALID"
        if self.gate_failures:
            return "FAIL"
        return "PASS"


def _records(lines: Sequence[str], marker: str) -> list[dict[str, str]]:
    return clock_analyzer._records(lines, marker)


def _one(lines: Sequence[str], marker: str, context: str,
         analysis: StressAnalysis) -> dict[str, str] | None:
    records = _records(lines, marker)
    if len(records) != 1:
        analysis.validation_errors.append(
            f"expected one {context}, found {len(records)}"
        )
        return records[0] if records else None
    return records[0]


def _integer(record: dict[str, str], name: str, context: str,
             analysis: StressAnalysis) -> int | None:
    value = record.get(name)
    if value is None:
        analysis.validation_errors.append(f"{context}: missing {name}")
        return None
    try:
        return clock_analyzer._integer(value)
    except ValueError:
        analysis.validation_errors.append(
            f"{context}: invalid {name}={value!r}"
        )
        return None


def _require_result(record: dict[str, str] | None, context: str,
                    analysis: StressAnalysis) -> None:
    if record is not None and record.get("result") != "PASS":
        analysis.gate_failures.append(
            f"{context}: result={record.get('result', '<missing>')}"
        )


def _validate_phases(lines: Sequence[str], analysis: StressAnalysis) -> None:
    marker = "c_ext_stress phase="
    phases = []
    current_phase = None
    for line_number, line in enumerate(lines, start=1):
        offset = line.find(marker)
        if offset >= 0:
            phase = line[offset + len(marker):].split()[0]
            phases.append(phase)
            current_phase = phase
        if "ble_hs_edisabled" in line.lower() and current_phase != "cleanup":
            analysis.gate_failures.append(
                f"line {line_number}: BLE_HS_EDISABLED outside cleanup"
            )
    analysis.phases = phases
    if tuple(phases) != PHASES:
        analysis.validation_errors.append(
            f"phase order={phases}, expected={list(PHASES)}"
        )


def _validate_config(lines: Sequence[str], analysis: StressAnalysis) -> None:
    record = _one(lines, "display config ", "display config", analysis)
    if record is None:
        return
    expected_int = {
        "qspi_hz": 40_000_000,
        "draw_rows": 60,
        "dma_rows": 10,
        "queue": 2,
        "direct": 0,
        "te": 0,
        "draw_units": 1,
        "draw_stack": 0,
        "adapter_stack": 32_768,
    }
    for name, expected in expected_int.items():
        value = _integer(record, name, "display config", analysis)
        if value is not None and value != expected:
            analysis.validation_errors.append(
                f"display config: {name}={value}, expected {expected}"
            )
    expected_text = {
        "color": "RGB565",
        "snapshot": "enabled",
        "lv_os": "none",
        "load_profile": "full",
    }
    for name, expected in expected_text.items():
        value = record.get(name)
        if value != expected:
            analysis.validation_errors.append(
                f"display config: {name}={value}, expected {expected}"
            )


def _validate_samples(lines: Sequence[str], analysis: StressAnalysis) -> None:
    records = [
        record for record in _records(lines, "c_ext_stress ")
        if "sample" in record
    ]
    if not records:
        analysis.validation_errors.append("expected stress samples")
        return
    required_psram_mask = 0x3E1
    for index, record in enumerate(records, start=1):
        context = f"sample {index}"
        if record.get("phase") not in ("warmup", "measure"):
            analysis.validation_errors.append(
                f"{context}: phase={record.get('phase')}"
            )
        missing = _integer(record, "task_missing", context, analysis)
        internal = _integer(record, "task_internal", context, analysis)
        dma_largest = _integer(record, "dma_largest", context, analysis)
        for name in (
            "internal_free",
            "internal_largest",
            "dma_free",
            "psram_free",
            "psram_largest",
        ):
            value = _integer(record, name, context, analysis)
            if value is not None and value <= 0:
                analysis.gate_failures.append(f"{context}: {name}={value}")
        if missing not in (None, 0):
            analysis.gate_failures.append(
                f"{context}: task_missing=0x{missing:x}"
            )
        if internal is not None and internal & required_psram_mask:
            analysis.gate_failures.append(
                f"{context}: required PSRAM task mask=0x{internal:x}"
            )
        if dma_largest is not None and dma_largest < DMA_LARGEST_MINIMUM:
            analysis.gate_failures.append(
                f"{context}: dma_largest={dma_largest}"
            )


def _validate_tasks(lines: Sequence[str], analysis: StressAnalysis) -> None:
    records = [
        record for record in _records(lines, "c_ext_stress ")
        if "task" in record and "min_hwm" in record
    ]
    by_name = {record.get("task", ""): record for record in records}
    if len(records) != len(TASKS) or set(by_name) != TASKS:
        analysis.validation_errors.append(
            f"task records={sorted(by_name)}, expected={sorted(TASKS)}"
        )
        return
    for name, record in by_name.items():
        context = f"task {name}"
        _require_result(record, context, analysis)
        minimum = _integer(record, "min_hwm", context, analysis)
        found = _integer(record, "found", context, analysis)
        psram = _integer(record, "stack_psram", context, analysis)
        required = 4096 if name == "lvgl" else 1024
        if minimum is not None and minimum < required:
            analysis.gate_failures.append(
                f"{context}: min_hwm={minimum} < {required}"
            )
        if found not in (None, 1):
            analysis.gate_failures.append(f"{context}: found={found}")
        if name in PSRAM_TASKS and psram not in (None, 1):
            analysis.gate_failures.append(f"{context}: stack_psram={psram}")


def _validate_summaries(lines: Sequence[str], analysis: StressAnalysis) -> None:
    ble = _one(lines, "c_ext_stress ble ", "BLE summary", analysis)
    audio = _one(lines, "c_ext_stress audio ", "audio summary", analysis)
    heap = _one(lines, "c_ext_stress heap ", "heap summary", analysis)
    stress = _one(lines, "c_ext_stress summary ", "stress summary", analysis)
    for context, record in (
        ("BLE summary", ble),
        ("audio summary", audio),
        ("heap summary", heap),
        ("stress summary", stress),
    ):
        _require_result(record, context, analysis)
    if ble is not None:
        for name in ("protected_failure", "disconnects", "reconnects"):
            value = _integer(ble, name, "BLE summary", analysis)
            if value not in (None, 0):
                analysis.gate_failures.append(f"BLE summary: {name}={value}")
        for name in ("max_success_interval_us", "max_success_idle_us"):
            value = _integer(ble, name, "BLE summary", analysis)
            if value is not None and value > 10_000_000:
                analysis.gate_failures.append(f"BLE summary: {name}={value}")
    if audio is not None:
        for name in (
            "tx_short",
            "rx_short",
            "tx_timeout",
            "rx_timeout",
            "tx_error",
            "rx_error",
            "tx_deadline_miss",
            "rx_deadline_miss",
            "faults",
        ):
            value = _integer(audio, name, "audio summary", analysis)
            if value not in (None, 0):
                analysis.gate_failures.append(
                    f"audio summary: {name}={value}"
                )
        mic = _integer(audio, "mic_nonzero", "audio summary", analysis)
        if mic not in (None, 1):
            analysis.gate_failures.append(f"audio summary: mic_nonzero={mic}")
    if heap is not None:
        dma = _integer(heap, "min_dma_largest", "heap summary", analysis)
        if dma is not None and dma < DMA_LARGEST_MINIMUM:
            analysis.gate_failures.append(
                f"heap summary: min_dma_largest={dma}"
            )
        for name in ("trend_ok", "recovery_ok"):
            value = _integer(heap, name, "heap summary", analysis)
            if value not in (None, 1):
                analysis.gate_failures.append(f"heap summary: {name}={value}")
    if stress is not None:
        if stress.get("performance") not in ("TARGET", "FLOOR"):
            analysis.gate_failures.append(
                f"stress summary: performance={stress.get('performance')}"
            )
        for name in ("completed", "navigation_ok", "tasks_ok"):
            value = _integer(stress, name, "stress summary", analysis)
            if value not in (None, 1):
                analysis.gate_failures.append(f"stress summary: {name}={value}")

    display = _one(
        lines, "display benchmark ", "display final", analysis
    )
    if display is not None:
        if display.get("stability") != "PASS":
            analysis.gate_failures.append(
                f"display final: stability={display.get('stability')}"
            )
        if display.get("performance") not in ("TARGET", "FLOOR"):
            analysis.gate_failures.append(
                f"display final: performance={display.get('performance')}"
            )
        fallbacks = _integer(
            display, "snapshot_fallbacks", "display final", analysis
        )
        p95 = _integer(
            display, "snapshot_prepare_max_p95_us", "display final", analysis
        )
        if fallbacks not in (None, 0):
            analysis.gate_failures.append(
                f"display final: snapshot_fallbacks={fallbacks}"
            )
        if p95 is not None and (p95 == 0 or p95 > SNAPSHOT_P95_MAXIMUM_US):
            analysis.gate_failures.append(
                f"display final: snapshot_prepare_max_p95_us={p95}"
            )
    load = _one(lines, "display load ", "load final", analysis)
    if load is not None:
        for name in (
            "tcp_rate_ok",
        ):
            value = _integer(load, name, "load final", analysis)
            if value not in (None, 1):
                analysis.gate_failures.append(f"load final: {name}={value}")
        for name in (
            "tcp_reconnects",
            "wifi_disconnects",
            "workload",
            "control",
            "audio",
            "tcp",
        ):
            value = _integer(load, name, "load final", analysis)
            if value not in (None, 0):
                analysis.gate_failures.append(f"load final: {name}={value}")


def analyze(path: Path) -> StressAnalysis:
    analysis = StressAnalysis(path=path)
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as error:
        analysis.validation_errors.append(f"cannot read log: {error}")
        return analysis
    lower_lines = [line.lower() for line in lines]
    for marker in FATAL_MARKERS:
        if any(marker in line for line in lower_lines):
            analysis.gate_failures.append(f"fatal marker: {marker}")
    _validate_phases(lines, analysis)
    _validate_config(lines, analysis)
    _validate_samples(lines, analysis)
    _validate_tasks(lines, analysis)
    _validate_summaries(lines, analysis)
    return analysis


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    analysis = analyze(args.log)
    payload = asdict(analysis)
    payload["path"] = str(analysis.path)
    payload["status"] = analysis.status
    print(json.dumps(payload, indent=2, sort_keys=True))
    return 0 if analysis.status == "PASS" else 2


if __name__ == "__main__":
    raise SystemExit(main())
