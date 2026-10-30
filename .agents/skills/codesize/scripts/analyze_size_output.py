#!/usr/bin/env python3
"""Archive size analyzer for the MicroTech ESP32-S3 firmware."""

from __future__ import annotations

import argparse
import csv
import json
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from datetime import datetime, timezone
from io import StringIO
from pathlib import Path
from typing import Dict, List

from analyze_map_file import classify_domain, normalize_library_identity
from codesize_utils import format_size

PROJECT_LABEL = "MicroTech"
TARGET_NAME = "microtech (ESP32-S3)"
SCHEMA_VERSION = 2
TOOL_VERSION = "2.0"


@dataclass
class ObjectInfo:
    """One archive member."""

    name: str
    library: str
    lib_path: str
    domain: str
    text: int
    data: int
    bss: int

    @property
    def total(self) -> int:
        return self.text + self.data + self.bss

    @property
    def flash(self) -> int:
        return self.text + self.data

    @property
    def ram(self) -> int:
        return self.data + self.bss


@dataclass
class LibraryStats:
    """Aggregated archive data."""

    name: str
    key: str = ""
    path: str = ""
    domain: str = "other"
    text: int = 0
    data: int = 0
    bss: int = 0
    objects: List[ObjectInfo] = field(default_factory=list)

    @property
    def total(self) -> int:
        return self.text + self.data + self.bss

    @property
    def flash(self) -> int:
        return self.text + self.data

    @property
    def ram(self) -> int:
        return self.data + self.bss


@dataclass
class AnalysisResult:
    """Archive analysis result."""

    objects: List[ObjectInfo] = field(default_factory=list)
    libraries: Dict[str, LibraryStats] = field(default_factory=dict)

    @property
    def total_text(self) -> int:
        return sum(item.text for item in self.objects)

    @property
    def total_data(self) -> int:
        return sum(item.data for item in self.objects)

    @property
    def total_bss(self) -> int:
        return sum(item.bss for item in self.objects)

    @property
    def total_flash(self) -> int:
        return self.total_text + self.total_data

    @property
    def total_ram(self) -> int:
        return self.total_data + self.total_bss

    @property
    def total_size(self) -> int:
        return self.total_text + self.total_data + self.total_bss


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def parse_size_file(filename: str) -> AnalysisResult:
    """Parse GNU `size -t` output for one or more archives."""
    result = AnalysisResult()
    libraries: Dict[str, LibraryStats] = {}

    with open(filename, "r", encoding="utf-8", errors="ignore") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line or line.startswith("text") or "(TOTALS)" in line:
                continue
            if "(ex " not in line:
                continue

            parts = line.split()
            if len(parts) < 6:
                continue
            try:
                text = int(parts[0])
                data = int(parts[1])
                bss = int(parts[2])
            except ValueError:
                continue

            object_info = " ".join(parts[5:])
            object_part, separator, library_part = object_info.partition("(ex ")
            if not separator:
                continue
            object_name = object_part.strip()
            library_path = library_part.rstrip(")").strip()
            if not object_name or not library_path:
                continue

            key, library_name = normalize_library_identity(library_path)
            domain = classify_domain(key, library_path, library_name)
            item = ObjectInfo(
                name=object_name,
                library=library_name,
                lib_path=library_path,
                domain=domain,
                text=text,
                data=data,
                bss=bss,
            )
            result.objects.append(item)

            stats = libraries.setdefault(key, LibraryStats(name=library_name, key=key, path=library_path, domain=domain))
            stats.text += text
            stats.data += data
            stats.bss += bss
            stats.objects.append(item)

    result.libraries = libraries
    return result


def generate_markdown_report(result: AnalysisResult, args: argparse.Namespace) -> str:
    """Generate the Markdown archive report."""
    lines: List[str] = []
    lines.append(f"# {PROJECT_LABEL} Archive Size Report")
    lines.append("")
    lines.append(f"- **Scope**: {args.module}")
    lines.append("")
    lines.append("## Overview")
    lines.append("")
    lines.append("```")
    lines.append("Flash usage (code and initialized data)")
    lines.append(f"  text (code):               {format_size(result.total_text):>12}")
    lines.append(f"  data (initialized data):   {format_size(result.total_data):>12}")
    lines.append(f"  Flash Total:               {format_size(result.total_flash):>12}")
    lines.append("")
    lines.append("RAM usage (runtime memory)")
    lines.append(f"  data (initialized data):   {format_size(result.total_data):>12}")
    lines.append(f"  bss (uninitialized data):  {format_size(result.total_bss):>12}")
    lines.append(f"  RAM Total:                 {format_size(result.total_ram):>12}")
    lines.append("")
    lines.append(f"Total archive bytes:         {format_size(result.total_size):>12}")
    lines.append("```")
    lines.append("")

    libraries = sorted(result.libraries.values(), key=lambda item: item.total, reverse=True)
    name_counts: Dict[str, int] = defaultdict(int)
    for library in libraries:
        name_counts[library.name] += 1

    def display_name(library: LibraryStats) -> str:
        return library.name if name_counts[library.name] == 1 else library.key

    lines.append("## By Library")
    lines.append("")
    lines.append("| Library | Domain | text | data | bss | Flash | RAM |")
    lines.append("|---------|--------|------|------|-----|-------|-----|")
    for library in libraries:
        lines.append(
            f"| {display_name(library)} | {library.domain} | {format_size(library.text)} | "
            f"{format_size(library.data)} | {format_size(library.bss)} | "
            f"{format_size(library.flash)} | {format_size(library.ram)} |"
        )
    lines.append("")

    lines.append("### Flash Share")
    lines.append("")
    lines.append("```")
    for library in libraries:
        percent = library.flash / result.total_flash * 100 if result.total_flash else 0
        bar = "#" * int(percent / 2)
        lines.append(f"{display_name(library):<60} {bar} {percent:5.1f}% ({format_size(library.flash)})")
    lines.append("```")
    lines.append("")

    lines.append(f"## Top {args.top} Object Files")
    lines.append("")
    lines.append("| Rank | Object | Library | Domain | Flash | RAM |")
    lines.append("|------|--------|---------|--------|-------|-----|")
    objects = sorted(result.objects, key=lambda item: item.total, reverse=True)
    for index, item in enumerate(objects[: args.top], 1):
        lines.append(
            f"| {index} | {item.name} | {item.library} | {item.domain} | "
            f"{format_size(item.flash)} | {format_size(item.ram)} |"
        )
    lines.append("")

    lines.append("## Optimization Guidance")
    lines.append("")
    large_objects = [item for item in objects if item.flash > args.threshold * 1024]
    if large_objects:
        lines.append(f"### 1. Large Object Files (Flash > {format_size(int(args.threshold * 1024))})")
        lines.append("")
        for item in large_objects[:10]:
            lines.append(f"- `{item.name}` ({item.library}) - Flash: {format_size(item.flash)}")
        lines.append("")

    data_objects = [item for item in objects if item.data > 100]
    if data_objects:
        lines.append("### 2. Initialized Data")
        lines.append("")
        for item in data_objects[:5]:
            lines.append(f"- `{item.name}` - data: {format_size(item.data)}")
        lines.append("")

    lines.append("### 3. Build and Link Checks")
    lines.append("")
    lines.append("- Compare archive totals with `build/microtech.map` attribution when an archive is linked.")
    lines.append("- Inspect archives that are pulled in as a whole instead of one object at a time.")
    lines.append("- Keep test, demo, and console components out of release images unless required.")
    lines.append("")
    return "\n".join(lines)


def generate_json_report(result: AnalysisResult) -> str:
    """Generate the JSON archive report."""
    report = {
        "schema_version": SCHEMA_VERSION,
        "tool_version": TOOL_VERSION,
        "project": PROJECT_LABEL,
        "target": TARGET_NAME,
        "generated_at": utc_now_iso(),
        "summary": {
            "total_text": result.total_text,
            "total_data": result.total_data,
            "total_bss": result.total_bss,
            "total_flash": result.total_flash,
            "total_ram": result.total_ram,
            "total_size": result.total_size,
        },
        "libraries": [
            {
                "key": library.key or library.name,
                "name": library.name,
                "path": library.path or library.name,
                "domain": library.domain,
                "text": library.text,
                "data": library.data,
                "bss": library.bss,
                "flash": library.flash,
                "ram": library.ram,
                "total": library.total,
                "object_count": len(library.objects),
            }
            for library in sorted(result.libraries.values(), key=lambda item: item.total, reverse=True)
        ],
        "objects": [
            {
                "name": item.name,
                "library": item.library,
                "lib_path": item.lib_path,
                "domain": item.domain,
                "text": item.text,
                "data": item.data,
                "bss": item.bss,
                "flash": item.flash,
                "ram": item.ram,
                "total": item.total,
            }
            for item in sorted(result.objects, key=lambda entry: entry.total, reverse=True)[:100]
        ],
    }
    return json.dumps(report, indent=2, ensure_ascii=False)


def generate_csv_report(result: AnalysisResult) -> str:
    output = StringIO()
    writer = csv.writer(output)
    writer.writerow(["Object File", "Library", "Domain", "text", "data", "bss", "flash", "ram", "total"])
    for item in sorted(result.objects, key=lambda entry: entry.total, reverse=True):
        writer.writerow(
            [
                item.name,
                item.library,
                item.domain,
                item.text,
                item.data,
                item.bss,
                item.flash,
                item.ram,
                item.total,
            ]
        )
    return output.getvalue()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Analyze GNU size output for MicroTech archives.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s /tmp/archives.size.txt
  %(prog)s /tmp/archives.size.txt --top 30 --format json
        """,
    )
    parser.add_argument("input_file", help="Path to `size -t` output")
    parser.add_argument("--top", type=int, default=15, help="Show Top N object files (default: 15)")
    parser.add_argument("--threshold", type=float, default=5.0, help="Large object threshold in KB (default: 5.0)")
    parser.add_argument("--output", "-o", help="Write the report to this file")
    parser.add_argument("--format", "-f", choices=["markdown", "json", "csv"], default="markdown")
    parser.add_argument("--module", default="Application archives", help="Scope label for the report")
    parser.add_argument("--fail-on-threshold", type=float, default=None, help="Fail if Flash exceeds this value in KB")
    args = parser.parse_args()

    try:
        result = parse_size_file(args.input_file)
        if not result.objects:
            print(f"Error: cannot parse object rows from {args.input_file}", file=sys.stderr)
            return 2
    except FileNotFoundError:
        print(f"Error: file not found - {args.input_file}", file=sys.stderr)
        return 2
    except Exception as exc:  # pragma: no cover - CLI boundary
        print(f"Error: {exc}", file=sys.stderr)
        return 2

    if args.format == "json":
        output = generate_json_report(result)
    elif args.format == "csv":
        output = generate_csv_report(result)
    else:
        output = generate_markdown_report(result, args)

    if args.output:
        try:
            Path(args.output).write_text(output, encoding="utf-8")
            print(f"Report generated: {args.output}", file=sys.stderr)
        except OSError as exc:
            print(f"Error: cannot write {args.output}: {exc}", file=sys.stderr)
            return 2
    else:
        print(output)

    if args.fail_on_threshold is not None:
        threshold = int(args.fail_on_threshold * 1024)
        if result.total_flash > threshold:
            print(f"Flash {format_size(result.total_flash)} exceeds threshold {format_size(threshold)}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
