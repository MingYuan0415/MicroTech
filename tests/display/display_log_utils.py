#!/usr/bin/env python3
"""Shared parsing constants and helpers for display benchmark logs."""

from __future__ import annotations

from typing import Sequence


EFFECTS = (
    "fade",
    "push-left",
    "push-right",
    "cover-left",
    "reveal-right",
)
LOADS = ("display-only", "full")
MEMORY_INTEGER_FIELDS = (
    "min_internal_free",
    "min_internal_largest",
    "min_dma_free",
    "min_dma_largest",
    "min_psram_free",
    "min_psram_largest",
    "render_task",
    "render_stack_psram",
    "render_stack_hwm",
    "render_core",
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


def parse_key_values(fragment: str) -> dict[str, str]:
    """Parse whitespace-separated key=value fields from one log suffix."""
    fields = {}
    for token in fragment.strip().split():
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        fields[key] = value.rstrip("\r")
    return fields


def integer(value: str) -> int:
    """Parse one decimal or signed hexadecimal log integer."""
    if value.lower().startswith(("0x", "+0x", "-0x")):
        return int(value, 16)
    return int(value, 10)


def records(lines: Sequence[str], marker: str) -> list[dict[str, str]]:
    """Return parsed key/value records following a marker."""
    parsed = []
    for line in lines:
        offset = line.find(marker)
        if offset >= 0:
            parsed.append(parse_key_values(line[offset + len(marker):]))
    return parsed
