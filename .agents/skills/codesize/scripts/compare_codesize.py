#!/usr/bin/env python3
"""Compare normalized code size reports for the MicroTech firmware."""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from dataclasses import dataclass, field
from datetime import datetime, timezone
from io import StringIO
from pathlib import Path
from typing import Any, Dict, List, Optional

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import analyze_map_file  # type: ignore


@dataclass
class ModuleStats:
    """Module statistics."""

    key: str
    name: str
    text: int = 0
    data: int = 0
    bss: int = 0
    rodata: int = 0
    path: str = ""
    domain: str = "other"
    sections: Dict[str, int] = field(default_factory=dict)
    flash_load: Optional[int] = None
    internal_ram: Optional[int] = None
    psram: int = 0
    l2ram: int = 0
    total_size: Optional[int] = None

    @property
    def flash(self) -> int:
        if self.flash_load is not None:
            return self.flash_load
        return self.text + self.data + self.rodata

    @property
    def ram(self) -> int:
        if self.internal_ram is not None:
            return self.internal_ram
        return self.data + self.bss

    @property
    def total(self) -> int:
        if self.total_size is not None:
            return self.total_size
        if self.sections:
            return sum(self.sections.values())
        return self.text + self.data + self.bss + self.rodata


@dataclass
class DiffResult:
    """Diff result."""

    key: str
    name: str
    before: ModuleStats
    after: ModuleStats

    @property
    def delta_text(self) -> int:
        return self.after.text - self.before.text

    @property
    def delta_data(self) -> int:
        return self.after.data - self.before.data

    @property
    def delta_bss(self) -> int:
        return self.after.bss - self.before.bss

    @property
    def delta_rodata(self) -> int:
        return self.after.rodata - self.before.rodata

    @property
    def delta_psram(self) -> int:
        return self.after.psram - self.before.psram

    @property
    def delta_l2ram(self) -> int:
        return self.after.l2ram - self.before.l2ram

    @property
    def delta_flash(self) -> int:
        return self.after.flash - self.before.flash

    @property
    def delta_ram(self) -> int:
        return self.after.ram - self.before.ram

    @property
    def delta_total(self) -> int:
        return self.after.total - self.before.total

    @property
    def percent_change(self) -> float:
        if self.before.total == 0:
            return 100.0 if self.after.total > 0 else 0.0
        return (self.delta_total / self.before.total) * 100


@dataclass
class CompareReport:
    """Comparison report."""

    before_file: str
    after_file: str
    before_total: ModuleStats
    after_total: ModuleStats
    diffs: List[DiffResult] = field(default_factory=list)
    added_modules: List[ModuleStats] = field(default_factory=list)
    removed_modules: List[ModuleStats] = field(default_factory=list)
    object_diffs: List[DiffResult] = field(default_factory=list)
    added_objects: List[ModuleStats] = field(default_factory=list)
    removed_objects: List[ModuleStats] = field(default_factory=list)
    symbol_diffs: List[DiffResult] = field(default_factory=list)
    added_symbols: List[ModuleStats] = field(default_factory=list)
    removed_symbols: List[ModuleStats] = field(default_factory=list)
    budget_violations: List[str] = field(default_factory=list)


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def format_size(size_bytes: int) -> str:
    if abs(size_bytes) < 1024:
        return f"{size_bytes} B"
    if abs(size_bytes) < 1024 * 1024:
        return f"{size_bytes / 1024:.2f} KB"
    return f"{size_bytes / (1024 * 1024):.2f} MB"


def format_delta(delta: int) -> str:
    if delta == 0:
        return "±0"
    sign = "+" if delta > 0 else ""
    return f"{sign}{format_size(delta)}"


def total_diff(before: int, after: int) -> int:
    return after - before


def calc_entry_flash(sections: Dict[str, int]) -> int:
    return analyze_map_file.calc_flash_load(sections)


def calc_entry_ram(sections: Dict[str, int]) -> int:
    return analyze_map_file.calc_internal_ram(sections)


def calc_entry_psram(sections: Dict[str, int]) -> int:
    return analyze_map_file.calc_psram(sections)


def calc_entry_l2ram(sections: Dict[str, int]) -> int:
    return analyze_map_file.calc_l2ram(sections)


def make_stats_from_json_entry(entry: Dict[str, Any], default_key: str = "unknown") -> ModuleStats:
    sections = {k: int(v) for k, v in entry.get("sections", {}).items()}
    key = entry.get("key") or entry.get("path") or entry.get("name") or default_key
    text = sections.get(".text", entry.get("text", 0))
    data = sections.get(".data", entry.get("data", 0))
    rodata = sections.get(".rodata", entry.get("rodata", 0))
    bss = (
        sections.get(".bss", 0)
        + sections.get(".reserved_ram", 0)
        + entry.get("bss", 0)
    )
    flash_load = entry.get("flash_load")
    internal_ram = entry.get("internal_ram")
    psram = entry.get("psram")
    l2ram = entry.get("l2ram")
    if flash_load is None and sections:
        flash_load = calc_entry_flash(sections)
    if internal_ram is None and sections:
        internal_ram = calc_entry_ram(sections)
    if psram is None and sections:
        psram = calc_entry_psram(sections)
    if l2ram is None and sections:
        l2ram = calc_entry_l2ram(sections)
    return ModuleStats(
        key=key,
        name=entry.get("name", key),
        path=entry.get("path", entry.get("library_key", key)),
        domain=entry.get("domain", "other"),
        text=text,
        data=data,
        bss=bss,
        rodata=rodata,
        sections=sections,
        flash_load=flash_load,
        internal_ram=internal_ram,
        psram=psram or 0,
        l2ram=l2ram or 0,
        total_size=entry.get("size") or entry.get("total"),
    )


def parse_json_file(filename: str) -> Dict[str, ModuleStats]:
    modules: Dict[str, ModuleStats] = {}
    with open(filename, "r", encoding="utf-8") as f:
        data = json.load(f)

    for lib in data.get("libraries", []):
        stat = make_stats_from_json_entry(lib)
        modules[stat.key] = stat

    summary = data.get("summary", {})
    if summary:
        sections = summary.get("sections", {})
        modules["__TOTAL__"] = ModuleStats(
            key="__TOTAL__",
            name="TOTAL",
            text=sections.get(".text", 0),
            data=sections.get(".data", 0),
            bss=sections.get(".bss", 0),
            rodata=sections.get(".rodata", 0),
            flash_load=summary.get("flash", 0),
            internal_ram=summary.get("internal_ram", 0),
            psram=summary.get("psram", 0),
            l2ram=summary.get("l2ram", 0),
            total_size=summary.get("accounted_sections", 0),
        )
    return modules


def parse_json_entries(filename: str, field_name: str) -> Dict[str, ModuleStats]:
    with open(filename, "r", encoding="utf-8") as f:
        data = json.load(f)
    entries = data.get(field_name, [])
    if field_name == "symbols" and not entries:
        entries = data.get("top_symbols", [])
    stats: Dict[str, ModuleStats] = {}
    for index, entry in enumerate(entries):
        stat = make_stats_from_json_entry(entry, f"{field_name}:{index}")
        if stat.key in stats:
            existing = stats[stat.key]
            existing.flash_load = (existing.flash_load or 0) + stat.flash
            existing.internal_ram = (existing.internal_ram or 0) + stat.ram
            existing.psram += stat.psram
            existing.l2ram += stat.l2ram
            existing.total_size = (existing.total_size or existing.total) + stat.total
            existing.text += stat.text
            existing.data += stat.data
            existing.bss += stat.bss
            existing.rodata += stat.rodata
            for section, size in stat.sections.items():
                existing.sections[section] = existing.sections.get(section, 0) + size
        else:
            stats[stat.key] = stat
    return stats


def parse_size_file(filename: str) -> Dict[str, ModuleStats]:
    modules: Dict[str, ModuleStats] = {}
    with open(filename, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("text") or "(TOTALS)" in line:
                continue
            parts = line.split()
            if len(parts) < 6 or "(ex " not in line:
                continue

            try:
                text = int(parts[0])
                data = int(parts[1])
                bss = int(parts[2])
            except ValueError:
                continue

            obj_info = " ".join(parts[5:])
            obj_match = obj_info.split("(ex ")
            if len(obj_match) != 2:
                continue

            lib_path = obj_match[1].rstrip(")")
            lib_key, lib_name = analyze_map_file.normalize_library_identity(lib_path)
            if lib_key not in modules:
                modules[lib_key] = ModuleStats(key=lib_key, name=lib_name, path=lib_path)
            modules[lib_key].text += text
            modules[lib_key].data += data
            modules[lib_key].bss += bss
    return modules


def parse_map_file(filename: str) -> Dict[str, ModuleStats]:
    parsed = analyze_map_file.parse_map_file(filename)
    modules: Dict[str, ModuleStats] = {}
    for key, stats in parsed.libraries.items():
        sections = dict(stats["sections"])
        modules[key] = ModuleStats(
            key=key,
            name=stats.get("name", key),
            path=stats.get("path", key),
            domain=stats.get("domain", "other"),
            text=sections.get(".text", 0),
            data=sections.get(".data", 0),
            bss=sections.get(".bss", 0) + sections.get(".reserved_ram", 0),
            rodata=sections.get(".rodata", 0),
            sections=sections,
            flash_load=calc_entry_flash(sections),
            internal_ram=calc_entry_ram(sections),
            psram=calc_entry_psram(sections),
            l2ram=calc_entry_l2ram(sections),
            total_size=stats.get("size"),
        )
    return modules


def compare_modules(before: Dict[str, ModuleStats], after: Dict[str, ModuleStats]) -> CompareReport:
    before_total = before.pop("__TOTAL__", None)
    after_total = after.pop("__TOTAL__", None)

    before_keys = set(before.keys())
    after_keys = set(after.keys())

    diffs: List[DiffResult] = []
    for key in before_keys & after_keys:
        diffs.append(DiffResult(key=key, name=after[key].name, before=before[key], after=after[key]))
    diffs.sort(key=lambda item: abs(item.delta_total), reverse=True)

    added_modules = [after[key] for key in after_keys - before_keys]
    removed_modules = [before[key] for key in before_keys - after_keys]
    added_modules.sort(key=lambda item: item.total, reverse=True)
    removed_modules.sort(key=lambda item: item.total, reverse=True)

    if before_total is None:
        before_total = ModuleStats(
            key="TOTAL",
            name="TOTAL",
            text=sum(m.text for m in before.values()),
            data=sum(m.data for m in before.values()),
            bss=sum(m.bss for m in before.values()),
            rodata=sum(m.rodata for m in before.values()),
            flash_load=sum(m.flash for m in before.values()),
            internal_ram=sum(m.ram for m in before.values()),
            psram=sum(m.psram for m in before.values()),
            l2ram=sum(m.l2ram for m in before.values()),
            total_size=sum(m.total for m in before.values()),
        )
    if after_total is None:
        after_total = ModuleStats(
            key="TOTAL",
            name="TOTAL",
            text=sum(m.text for m in after.values()),
            data=sum(m.data for m in after.values()),
            bss=sum(m.bss for m in after.values()),
            rodata=sum(m.rodata for m in after.values()),
            flash_load=sum(m.flash for m in after.values()),
            internal_ram=sum(m.ram for m in after.values()),
            psram=sum(m.psram for m in after.values()),
            l2ram=sum(m.l2ram for m in after.values()),
            total_size=sum(m.total for m in after.values()),
        )

    return CompareReport(
        before_file="",
        after_file="",
        before_total=before_total,
        after_total=after_total,
        diffs=diffs,
        added_modules=added_modules,
        removed_modules=removed_modules,
    )


def build_project_suggestions(report: CompareReport) -> List[str]:
    suggestions: List[str] = []
    growth = [diff for diff in report.diffs if diff.delta_flash > 0][:10]

    if any("lvgl" in diff.name.lower() or "freetype" in diff.name.lower() for diff in growth):
        suggestions.append("UI/graphics growth is visible; inspect newly linked widgets, fonts, and image decoders.")
    if any("bt" in diff.name.lower() or "ble" in diff.name.lower() for diff in growth):
        suggestions.append("Bluetooth growth is visible; inspect the NimBLE role set, GATT profiles, and controller buffers.")
    if any(diff.name in {"libc.a", "libm.a"} for diff in growth):
        suggestions.append("Toolchain libraries changed; inspect float formatting, scanf calls, and heavy math call sites.")
    if any("mbedtls" in diff.name.lower() or "lwip" in diff.name.lower() for diff in growth):
        suggestions.append("Network/TLS growth is visible; inspect mbedTLS features and the certificate bundle.")

    ram_grow = [
        diff for diff in report.diffs
        if diff.delta_ram > 0 and diff.name in {"libfreertos.a", "libesp_system.a", "libesp_libc.a"}
    ]
    if ram_grow:
        detail = ", ".join(f"{diff.name}={format_size(diff.delta_ram)}" for diff in ram_grow)
        suggestions.append(f"Runtime library RAM growth: {detail}; inspect static buffers and initial state.")

    return suggestions


def build_budget_violations(report: CompareReport, budget_file: Optional[str]) -> List[str]:
    if not budget_file:
        return []
    with open(budget_file, "r", encoding="utf-8") as f:
        budget = json.load(f)

    violations: List[str] = []
    total_change = DiffResult(key="TOTAL", name="TOTAL", before=report.before_total, after=report.after_total)
    for budget_key, label, delta in (
        ("max_flash_growth", "Flash growth", total_change.delta_flash),
        ("max_ram_growth", "Internal RAM growth", total_change.delta_ram),
        ("max_psram_growth", "PSRAM growth", total_change.delta_psram),
    ):
        max_growth = budget.get(budget_key)
        if max_growth is not None and delta > int(max_growth):
            violations.append(
                f"{label} {format_delta(delta)} exceeds budget {format_size(int(max_growth))}"
            )

    library_deltas: Dict[str, Dict[str, Any]] = {}
    library_names: Dict[str, str] = {}
    for diff in report.diffs:
        library_deltas[diff.key] = {
            "name": diff.name,
            "flash": diff.delta_flash,
            "ram": diff.delta_ram,
        }
        library_names[diff.name] = diff.key
    for module in report.added_modules:
        library_deltas[module.key] = {"name": module.name, "flash": module.flash, "ram": module.ram}
        library_names[module.name] = module.key
    for module in report.removed_modules:
        library_deltas[module.key] = {"name": module.name, "flash": -module.flash, "ram": -module.ram}
        library_names[module.name] = module.key

    for key, rule in budget.get("libraries", {}).items():
        resolved_key = key if key in library_deltas else library_names.get(key)
        if not resolved_key:
            continue
        delta = library_deltas[resolved_key]
        max_flash = rule.get("max_flash_growth")
        if max_flash is not None and delta["flash"] > int(max_flash):
            violations.append(
                f"Library {delta['name']} flash growth {format_delta(delta['flash'])} exceeds budget {format_size(int(max_flash))}"
            )
        max_ram = rule.get("max_ram_growth")
        if max_ram is not None and delta["ram"] > int(max_ram):
            violations.append(
                f"Library {delta['name']} RAM growth {format_delta(delta['ram'])} exceeds budget {format_size(int(max_ram))}"
            )

    domain_flash: Dict[str, int] = {}
    domain_ram: Dict[str, int] = {}
    for diff in report.diffs:
        domain = diff.after.domain or diff.before.domain or "other"
        domain_flash[domain] = domain_flash.get(domain, 0) + diff.delta_flash
        domain_ram[domain] = domain_ram.get(domain, 0) + diff.delta_ram
    for module in report.added_modules:
        domain = module.domain or "other"
        domain_flash[domain] = domain_flash.get(domain, 0) + module.flash
        domain_ram[domain] = domain_ram.get(domain, 0) + module.ram
    for module in report.removed_modules:
        domain = module.domain or "other"
        domain_flash[domain] = domain_flash.get(domain, 0) - module.flash
        domain_ram[domain] = domain_ram.get(domain, 0) - module.ram

    for domain, rule in budget.get("domains", {}).items():
        max_flash = rule.get("max_flash_growth")
        if max_flash is not None and domain_flash.get(domain, 0) > int(max_flash):
            violations.append(
                f"Domain {domain} flash growth {format_delta(domain_flash[domain])} exceeds budget {format_size(int(max_flash))}"
            )
        max_ram = rule.get("max_ram_growth")
        if max_ram is not None and domain_ram.get(domain, 0) > int(max_ram):
            violations.append(
                f"Domain {domain} RAM growth {format_delta(domain_ram[domain])} exceeds budget {format_size(int(max_ram))}"
            )

    return violations


def localize_budget_violation(message: str) -> str:
    """Return a budget violation message unchanged."""
    return message


def append_diff_table(lines: List[str], title: str, diffs: List[DiffResult], top_n: int) -> None:
    if not diffs:
        return
    lines.append(title)
    lines.append("")
    lines.append("| Rank | Name | Domain | Total Delta | Flash Delta | RAM Delta | PSRAM Delta |")
    lines.append("|------|------|--------|-------------|-------------|-----------|-------------|")
    for index, diff in enumerate(diffs[:top_n], 1):
        lines.append(
            f"| {index} | {diff.name} | {diff.after.domain or diff.before.domain} | {format_delta(diff.delta_total)} | "
            f"{format_delta(diff.delta_flash)} | {format_delta(diff.delta_ram)} | {format_delta(diff.delta_psram)} |"
        )
    lines.append("")


def append_added_removed_table(lines: List[str], title: str, modules: List[ModuleStats], top_n: int) -> None:
    if not modules:
        return
    lines.append(title)
    lines.append("")
    lines.append("| Name | Domain | Total | Flash | RAM |")
    lines.append("|------|--------|-------|-------|-----|")
    for module in modules[:top_n]:
        lines.append(
            f"| {module.name} | {module.domain or 'other'} | {format_size(module.total)} | "
            f"{format_size(module.flash)} | {format_size(module.ram)} |"
        )
    lines.append("")


def generate_markdown_report(report: CompareReport, threshold: float = 5.0, top_n: int = 20) -> str:
    lines: List[str] = []
    lines.append("# MicroTech Code Size Comparison Report")
    lines.append("")
    lines.append(f"- **Baseline**: {report.before_file}")
    lines.append(f"- **Compared**: {report.after_file}")
    lines.append("")

    total_change = DiffResult(key="TOTAL", name="TOTAL", before=report.before_total, after=report.after_total)

    lines.append("## Total Change")
    lines.append("")
    lines.append("| Metric | Baseline | Compared | Delta | Percent |")
    lines.append("|--------|----------|----------|-------|---------|")
    for metric, before_val, after_val, delta in [
        ("Flash", report.before_total.flash, report.after_total.flash, total_change.delta_flash),
        ("RAM", report.before_total.ram, report.after_total.ram, total_change.delta_ram),
        (".text", report.before_total.text, report.after_total.text, total_change.delta_text),
        (".rodata", report.before_total.rodata, report.after_total.rodata, total_change.delta_rodata),
        (".data", report.before_total.data, report.after_total.data, total_change.delta_data),
        (".bss", report.before_total.bss, report.after_total.bss, total_change.delta_bss),
        ("PSRAM", report.before_total.psram, report.after_total.psram, total_change.delta_psram),
    ]:
        percent = (delta / before_val * 100) if before_val > 0 else 0
        marker = "!!" if delta > 0 and abs(percent) >= threshold else ("--" if delta < 0 and abs(percent) >= threshold else "")
        percent_text = f"{percent:+.1f}%" if delta != 0 else "0%"
        lines.append(f"| {metric} | {format_size(before_val)} | {format_size(after_val)} | {format_delta(delta)} | {marker} {percent_text} |")
    lines.append("")

    if report.budget_violations:
        lines.append("## Budget Violations")
        lines.append("")
        for item in report.budget_violations:
            lines.append(f"- {localize_budget_violation(item)}")
        lines.append("")

    significant = [diff for diff in report.diffs if abs(diff.percent_change) >= threshold]
    if significant:
        lines.append(f"## Significant Changes (>{threshold}%)")
        lines.append("")
        lines.append("| Library | Baseline | Compared | Delta | Percent |")
        lines.append("|---------|----------|----------|-------|---------|")
        for diff in significant[:top_n]:
            marker = "!!" if diff.delta_total > 0 else "--"
            lines.append(f"| {diff.name} | {format_size(diff.before.total)} | {format_size(diff.after.total)} | {format_delta(diff.delta_total)} | {marker} {diff.percent_change:+.1f}% |")
        lines.append("")

    if report.added_modules:
        lines.append("## Added Libraries")
        lines.append("")
        lines.append("| Library | Size | Flash | RAM |")
        lines.append("|---------|------|-------|-----|")
        for module in report.added_modules[:10]:
            lines.append(f"| {module.name} | {format_size(module.total)} | {format_size(module.flash)} | {format_size(module.ram)} |")
        lines.append("")

    if report.removed_modules:
        lines.append("## Removed Libraries")
        lines.append("")
        lines.append("| Library | Size | Flash | RAM |")
        lines.append("|---------|------|-------|-----|")
        for module in report.removed_modules[:10]:
            lines.append(f"| {module.name} | {format_size(module.total)} | {format_size(module.flash)} | {format_size(module.ram)} |")
        lines.append("")

    append_added_removed_table(lines, "Added Objects", report.added_objects, top_n)
    append_added_removed_table(lines, "Removed Objects", report.removed_objects, top_n)
    append_added_removed_table(lines, "Added Symbols", report.added_symbols, top_n)
    append_added_removed_table(lines, "Removed Symbols", report.removed_symbols, top_n)

    lines.append(f"## Top {top_n} Library Changes")
    lines.append("")
    lines.append("| Rank | Library | Domain | Baseline | Compared | Total Delta | Flash Delta | RAM Delta | PSRAM Delta |")
    lines.append("|------|---------|--------|----------|----------|-------------|-------------|-----------|-------------|")
    for index, diff in enumerate(report.diffs[:top_n], 1):
        lines.append(
            f"| {index} | {diff.name} | {diff.after.domain or diff.before.domain} | "
            f"{format_size(diff.before.total)} | {format_size(diff.after.total)} | "
            f"{format_delta(diff.delta_total)} | {format_delta(diff.delta_flash)} | "
            f"{format_delta(diff.delta_ram)} | {format_delta(diff.delta_psram)} |"
        )
    lines.append("")

    append_diff_table(lines, f"## Top {top_n} Object Changes", report.object_diffs, top_n)
    append_diff_table(lines, f"## Top {top_n} Symbol Changes", report.symbol_diffs, top_n)

    lines.append("## Analysis")
    lines.append("")
    if total_change.delta_flash > 0:
        lines.append(f"- Flash increased by {format_size(total_change.delta_flash)}; inspect the libraries with the largest positive Flash delta.")
    elif total_change.delta_flash < 0:
        lines.append(f"- Flash decreased by {format_size(abs(total_change.delta_flash))}.")
    if total_change.delta_ram > 0:
        lines.append(f"- RAM increased by {format_size(total_change.delta_ram)}; inspect static allocation pressure.")
    elif total_change.delta_ram < 0:
        lines.append(f"- RAM decreased by {format_size(abs(total_change.delta_ram))}.")
    for item in build_project_suggestions(report):
        lines.append(f"- {item}")
    lines.append("")

    return "\n".join(lines)


def generate_json_report(report: CompareReport) -> str:
    def diff_to_json(diff: DiffResult) -> Dict[str, Any]:
        return {
            "key": diff.key,
            "name": diff.name,
            "domain": diff.after.domain or diff.before.domain,
            "before_total": diff.before.total,
            "after_total": diff.after.total,
            "delta_total": diff.delta_total,
            "delta_flash": diff.delta_flash,
            "delta_ram": diff.delta_ram,
            "delta_psram": diff.delta_psram,
            "delta_l2ram": diff.delta_l2ram,
            "percent_change": round(diff.percent_change, 2),
        }

    data = {
        "schema_version": 2,
        "tool_version": "2.0",
        "project": "MicroTech",
        "generated_at": utc_now_iso(),
        "before_file": report.before_file,
        "after_file": report.after_file,
        "summary": {
            "before": {
                "flash": report.before_total.flash,
                "ram": report.before_total.ram,
                "psram": report.before_total.psram,
                "l2ram": report.before_total.l2ram,
                "text": report.before_total.text,
                "data": report.before_total.data,
                "bss": report.before_total.bss,
                "rodata": report.before_total.rodata,
            },
            "after": {
                "flash": report.after_total.flash,
                "ram": report.after_total.ram,
                "psram": report.after_total.psram,
                "l2ram": report.after_total.l2ram,
                "text": report.after_total.text,
                "data": report.after_total.data,
                "bss": report.after_total.bss,
                "rodata": report.after_total.rodata,
            },
            "delta": {
                "flash": total_diff(report.before_total.flash, report.after_total.flash),
                "ram": total_diff(report.before_total.ram, report.after_total.ram),
                "psram": total_diff(report.before_total.psram, report.after_total.psram),
                "l2ram": total_diff(report.before_total.l2ram, report.after_total.l2ram),
                "text": total_diff(report.before_total.text, report.after_total.text),
                "data": total_diff(report.before_total.data, report.after_total.data),
                "bss": total_diff(report.before_total.bss, report.after_total.bss),
                "rodata": total_diff(report.before_total.rodata, report.after_total.rodata),
            },
        },
        "budget_violations": report.budget_violations,
        "modules": [diff_to_json(diff) for diff in report.diffs],
        "objects": [diff_to_json(diff) for diff in report.object_diffs],
        "symbols": [diff_to_json(diff) for diff in report.symbol_diffs],
        "added": [{"key": mod.key, "name": mod.name, "total": mod.total} for mod in report.added_modules],
        "removed": [{"key": mod.key, "name": mod.name, "total": mod.total} for mod in report.removed_modules],
        "added_objects": [{"key": mod.key, "name": mod.name, "total": mod.total} for mod in report.added_objects],
        "removed_objects": [{"key": mod.key, "name": mod.name, "total": mod.total} for mod in report.removed_objects],
        "added_symbols": [{"key": mod.key, "name": mod.name, "total": mod.total} for mod in report.added_symbols],
        "removed_symbols": [{"key": mod.key, "name": mod.name, "total": mod.total} for mod in report.removed_symbols],
    }
    return json.dumps(data, indent=2, ensure_ascii=False)


def generate_csv_report(report: CompareReport) -> str:
    output = StringIO()
    writer = csv.writer(output)
    writer.writerow([
        "Module Key", "Module", "Domain", "Baseline Size", "Compared Size", "Delta",
        "Change Percentage", "Flash Change", "RAM Change", "PSRAM Change", "L2 RAM Change", "Status"
    ])
    for diff in report.diffs:
        status = "increased" if diff.delta_total > 0 else ("decreased" if diff.delta_total < 0 else "unchanged")
        writer.writerow([
            diff.key,
            diff.name,
            diff.after.domain or diff.before.domain,
            diff.before.total,
            diff.after.total,
            diff.delta_total,
            f"{diff.percent_change:.2f}%",
            diff.delta_flash,
            diff.delta_ram,
            diff.delta_psram,
            diff.delta_l2ram,
            status,
        ])
    return output.getvalue()


def detect_and_parse(filepath: str, file_type: str) -> Dict[str, ModuleStats]:
    chosen = file_type
    if chosen == "auto":
        if filepath.endswith(".json"):
            chosen = "json"
        elif filepath.endswith(".map"):
            chosen = "map"
        else:
            with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
                first_line = f.readline().strip()
            if first_line.startswith("{"):
                chosen = "json"
            elif "text" in first_line and "data" in first_line:
                chosen = "size"
            else:
                chosen = "map"

    if chosen == "json":
        return parse_json_file(filepath)
    if chosen == "map":
        return parse_map_file(filepath)
    return parse_size_file(filepath)


def details_available(filepath: str, file_type: str) -> bool:
    return file_type == "json" or (file_type == "auto" and filepath.endswith(".json"))


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare code size between two versions",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s before.txt after.txt
  %(prog)s before.map after.map --type map
  %(prog)s before.json after.json --type json --format markdown
        """,
    )
    parser.add_argument("before", help="Baseline version file")
    parser.add_argument("after", help="Compared version file")
    parser.add_argument("--type", "-t", choices=["size", "map", "json"], default="auto", help="File type")
    parser.add_argument("--format", "-f", choices=["markdown", "json", "csv"], default="markdown", help="Output format")
    parser.add_argument("--output", "-o", help="Output file path")
    parser.add_argument("--threshold", type=float, default=5.0, help="Significant change threshold percentage")
    parser.add_argument("--top", type=int, default=20, help="Show Top N changed modules")
    parser.add_argument("--before-label", help="Display label for the baseline input")
    parser.add_argument("--after-label", help="Display label for the compared input")
    parser.add_argument("--fail-on-growth", type=float, default=None, help="Return non-zero exit code if Flash growth exceeds this percentage")
    parser.add_argument("--object-diff", action="store_true", help="Include object-level deltas when normalized JSON contains objects")
    parser.add_argument("--symbol-diff", action="store_true", help="Include symbol-level deltas when normalized JSON contains symbols")
    parser.add_argument("--budget", help="Optional JSON budget file for Flash/RAM growth checks")
    parser.add_argument("--fail-on-budget", action="store_true", help="Return non-zero if --budget violations are found")

    args = parser.parse_args()

    for filepath in [args.before, args.after]:
        if not Path(filepath).exists():
            print(f"Error: file not found - {filepath}", file=sys.stderr)
            return 2

    try:
        print(f"Parsing baseline: {args.before}", file=sys.stderr)
        before_modules = detect_and_parse(args.before, args.type)
        print(f"Parsing compared version: {args.after}", file=sys.stderr)
        after_modules = detect_and_parse(args.after, args.type)
        if not before_modules and not after_modules:
            print("Error: cannot parse module info from files", file=sys.stderr)
            return 2
    except Exception as exc:
        print(f"Error: failed to parse files - {exc}", file=sys.stderr)
        return 2

    print("Generating comparison report...", file=sys.stderr)
    report = compare_modules(before_modules, after_modules)
    report.before_file = args.before_label or args.before
    report.after_file = args.after_label or args.after
    if args.object_diff:
        before_objects = parse_json_entries(args.before, "objects") if details_available(args.before, args.type) else {}
        after_objects = parse_json_entries(args.after, "objects") if details_available(args.after, args.type) else {}
        if before_objects or after_objects:
            object_report = compare_modules(before_objects, after_objects)
            report.object_diffs = object_report.diffs
            report.added_objects = object_report.added_modules
            report.removed_objects = object_report.removed_modules
    if args.symbol_diff:
        before_symbols = parse_json_entries(args.before, "symbols") if details_available(args.before, args.type) else {}
        after_symbols = parse_json_entries(args.after, "symbols") if details_available(args.after, args.type) else {}
        if before_symbols or after_symbols:
            symbol_report = compare_modules(before_symbols, after_symbols)
            report.symbol_diffs = symbol_report.diffs
            report.added_symbols = symbol_report.added_modules
            report.removed_symbols = symbol_report.removed_modules
    try:
        report.budget_violations = build_budget_violations(report, args.budget)
    except (OSError, ValueError, json.JSONDecodeError, TypeError, AttributeError, KeyError) as exc:
        print(f"Error: cannot parse budget file - {exc}", file=sys.stderr)
        return 2

    if args.format == "json":
        output = generate_json_report(report)
    elif args.format == "csv":
        output = generate_csv_report(report)
    else:
        output = generate_markdown_report(report, args.threshold, args.top)

    if args.output:
        try:
            with open(args.output, "w", encoding="utf-8") as f:
                f.write(output)
        except OSError as exc:
            print(f"Error: cannot write {args.output}: {exc}", file=sys.stderr)
            return 2
        print(f"Report generated: {args.output}", file=sys.stderr)
    else:
        print(output)

    if args.fail_on_growth is not None and report.before_total.flash > 0:
        growth_pct = (report.after_total.flash - report.before_total.flash) / report.before_total.flash * 100
        if growth_pct > args.fail_on_growth:
            print(f"Flash growth {growth_pct:.1f}% exceeds threshold {args.fail_on_growth}%", file=sys.stderr)
            return 1
    if args.fail_on_budget and report.budget_violations:
        print("Budget violations found", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
