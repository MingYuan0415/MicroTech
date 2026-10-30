#!/usr/bin/env python3
"""Shared helpers for MicroTech code size analysis."""

from __future__ import annotations

from enum import Enum


class Architecture(Enum):
    """Linker map architecture."""

    ARM = "arm"
    XTENSA = "xtensa"
    RISCV = "riscv"
    UNKNOWN = "unknown"


class SectionType(Enum):
    """Runtime accounting buckets used by the MicroTech target."""

    TEXT = ".text"
    IRAM_TEXT = ".iram_text"
    RTC_TEXT = ".rtc_text"
    RODATA = ".rodata"
    DATA = ".data"
    BSS = ".bss"
    PSRAM_BSS = ".psram_bss"
    PSRAM_DATA = ".psram_data"
    RESERVED = ".reserved_ram"
    OTHER = "other"


def format_size(size_bytes: int) -> str:
    """Format a byte count as B, KB, or MB."""
    if abs(size_bytes) < 1024:
        return f"{size_bytes} B"
    if abs(size_bytes) < 1024 * 1024:
        return f"{size_bytes / 1024:.2f} KB"
    return f"{size_bytes / (1024 * 1024):.2f} MB"


def format_delta(delta: int) -> str:
    """Format a signed byte count."""
    if delta == 0:
        return "+/-0"
    sign = "+" if delta > 0 else ""
    return f"{sign}{format_size(delta)}"


def format_percentage(value: int, total: int, decimals: int = 1) -> str:
    """Format a percentage."""
    if total == 0:
        return "0%"
    return f"{value / total * 100:.{decimals}f}%"


def detect_architecture(content: str) -> Architecture:
    """Detect the target architecture from linker map or ELF content."""
    lowered = content.lower()
    if ".xt." in lowered or "xtensa" in lowered:
        return Architecture.XTENSA
    if ".riscv." in lowered:
        return Architecture.RISCV
    if ".arm." in lowered or "arm.attributes" in lowered:
        return Architecture.ARM
    return Architecture.UNKNOWN
