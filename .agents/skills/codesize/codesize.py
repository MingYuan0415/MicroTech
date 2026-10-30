#!/usr/bin/env python3
"""Command-line entry point for the MicroTech code size workflow."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from datetime import datetime
from io import StringIO
from pathlib import Path
from typing import Dict, List, Optional

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_LABEL = "MicroTech"
AUTO_REPORT_DIR = "__auto__"


def is_project_root(path: Path) -> bool:
    return (
        (path / "CMakeLists.txt").is_file()
        and (path / "main" / "CMakeLists.txt").is_file()
        and (path / "layers").is_dir()
    )


def find_project_root() -> Path:
    """Locate the MicroTech repository root from the working directory or skill path."""
    for base in (Path.cwd(), SCRIPT_DIR):
        for path in [base, *base.parents]:
            if is_project_root(path):
                return path
    return SCRIPT_DIR.parents[2]


ROOT_DIR = find_project_root()
DEFAULT_BUILD_DIR = ROOT_DIR / "build"


def script_path(name: str) -> Path:
    return SCRIPT_DIR / "scripts" / name


def run_python(script: Path, args: List[str]) -> int:
    return subprocess.run([sys.executable, str(script), *args], cwd=ROOT_DIR).returncode


def run_capture(command: List[str], output_file: Optional[Path] = None) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, cwd=ROOT_DIR, text=True, capture_output=True)
    if output_file is not None and result.returncode == 0:
        output_file.write_text(result.stdout, encoding="utf-8")
    return result


def timestamp() -> str:
    return datetime.now().strftime("%Y%m%d_%H%M%S")


def sanitize_name(name: str) -> str:
    kept = [char if char.isalnum() or char in ("-", "_", ".") else "_" for char in name]
    return "".join(kept).strip("_") or "report"


def comparison_label(path: Path) -> str:
    """Return a stable, unique label for one comparison input."""
    resolved = str(path.resolve())
    digest = hashlib.sha1(resolved.encode("utf-8")).hexdigest()[:8]
    parent = sanitize_name(path.parent.name) if path.parent.name not in ("", ".") else "root"
    return f"{parent}_{sanitize_name(path.stem)}_{digest}"


def load_project_description(build_dir: Path) -> Dict[str, str]:
    description = build_dir / "project_description.json"
    if not description.is_file():
        return {}
    try:
        return json.loads(description.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def default_map_path(build_dir: Path) -> Path:
    description = load_project_description(build_dir)
    app_elf = description.get("app_elf", "microtech.elf")
    candidate = build_dir / f"{Path(app_elf).stem}.map"
    if candidate.is_file():
        return candidate
    fallback = [path for path in sorted(build_dir.glob("*.map")) if path.is_file()]
    if fallback:
        return fallback[0]
    raise FileNotFoundError(f"no application map file found under {build_dir}")


def find_size_tool(build_dir: Path) -> Optional[Path]:
    description = load_project_description(build_dir)
    prefix = description.get("monitor_toolprefix", "xtensa-esp32s3-elf-")
    name = f"{prefix}size" if prefix else "xtensa-esp32s3-elf-size"
    found = shutil.which(name)
    if found:
        return Path(found)
    tools_root = Path.home() / ".espressif" / "tools"
    if tools_root.is_dir():
        for path in tools_root.rglob(name):
            if path.is_file():
                return path
    return None


def collect_archives(build_dir: Path) -> List[Path]:
    archives = [path for path in build_dir.glob("esp-idf/**/*.a") if path.is_file()]
    archives.extend(path for path in build_dir.glob("*.a") if path.is_file())
    return sorted(archives)


def analyze_current(args: argparse.Namespace) -> int:
    build_dir = Path(args.build_dir).resolve()
    try:
        map_path = Path(args.map).resolve() if args.map else default_map_path(build_dir)
    except FileNotFoundError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 2
    if not map_path.is_file():
        print(f"Error: map file not found - {map_path}", file=sys.stderr)
        return 2

    command = [
        str(map_path),
        "--top",
        str(args.top),
        "--threshold",
        str(args.threshold),
        "--format",
        args.format,
        "--group-by",
        args.group_by,
    ]
    if args.show_symbols:
        command.append("--show-symbols")
    if args.output:
        command.extend(["--output", str(Path(args.output).resolve())])
    if args.fail_on_threshold is not None:
        command.extend(["--fail-on-threshold", str(args.fail_on_threshold)])
    return run_python(script_path("analyze_map_file.py"), command)


def analyze_archives(args: argparse.Namespace) -> int:
    build_dir = Path(args.build_dir).resolve()
    archives = collect_archives(build_dir)
    if not archives:
        print(f"Error: no archives found under {build_dir}", file=sys.stderr)
        return 2

    size_tool = find_size_tool(build_dir)
    if size_tool is None:
        print("Error: cannot locate the toolchain size command; source the ESP-IDF export script", file=sys.stderr)
        return 2

    with tempfile.NamedTemporaryFile("w+", suffix=".size.txt", delete=False) as handle:
        temporary = Path(handle.name)
    try:
        command = [str(size_tool), "-t", *[str(path) for path in archives]]
        result = run_capture(command, temporary)
        if result.returncode != 0:
            sys.stderr.write(result.stderr)
            return result.returncode

        report_args = [
            str(temporary),
            "--top",
            str(args.top),
            "--threshold",
            str(args.threshold),
            "--format",
            args.format,
            "--module",
            f"{len(archives)} archives under {build_dir.name}",
        ]
        if args.output:
            report_args.extend(["--output", str(Path(args.output).resolve())])
        if args.fail_on_threshold is not None:
            report_args.extend(["--fail-on-threshold", str(args.fail_on_threshold)])
        return run_python(script_path("analyze_size_output.py"), report_args)
    finally:
        temporary.unlink(missing_ok=True)


def normalize_map_to_json(map_path: Path, output_path: Path, all_symbols: bool) -> int:
    command = [
        str(map_path),
        "--format",
        "json",
        "--output",
        str(output_path),
    ]
    if all_symbols:
        command.append("--all-symbols")
    return subprocess.run([sys.executable, str(script_path("analyze_map_file.py")), *command], cwd=ROOT_DIR).returncode


def resolve_comparison_input(value: str) -> Path:
    path = Path(value).resolve()
    if path.is_dir():
        description = load_project_description(path)
        app_elf = description.get("app_elf", "microtech.elf")
        candidate = path / f"{Path(app_elf).stem}.map"
        if candidate.is_file():
            return candidate
        maps = [item for item in sorted(path.glob("*.map")) if item.is_file()]
        if maps:
            return maps[0]
        raise FileNotFoundError(f"no map file found in {path}")
    if not path.is_file():
        raise FileNotFoundError(f"comparison input not found: {path}")
    return path


def compare_command_args(
    before_json: Path,
    after_json: Path,
    args: argparse.Namespace,
    output_format: str,
    output: Optional[Path] = None,
    before_label: Optional[str] = None,
    after_label: Optional[str] = None,
) -> List[str]:
    command = [
        str(before_json),
        str(after_json),
        "--type",
        "json",
        "--format",
        output_format,
        "--top",
        str(args.top),
        "--threshold",
        str(args.threshold),
    ]
    if before_label:
        command.extend(["--before-label", before_label])
    if after_label:
        command.extend(["--after-label", after_label])
    if args.object_diff:
        command.append("--object-diff")
    if args.symbol_diff:
        command.append("--symbol-diff")
    if args.budget:
        command.extend(["--budget", str(Path(args.budget).resolve())])
    if args.fail_on_budget:
        command.append("--fail-on-budget")
    if getattr(args, "fail_on_growth", None) is not None:
        command.extend(["--fail-on-growth", str(args.fail_on_growth)])
    if output:
        command.extend(["--output", str(output)])
    return command


def compare_builds(args: argparse.Namespace) -> int:
    try:
        before_path = resolve_comparison_input(args.before)
        after_path = resolve_comparison_input(args.after)
    except FileNotFoundError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="codesize_") as temporary:
        temporary_dir = Path(temporary)
        before_json = temporary_dir / f"before_{sanitize_name(before_path.stem)}.json"
        after_json = temporary_dir / f"after_{sanitize_name(after_path.stem)}.json"
        if normalize_map_to_json(before_path, before_json, args.symbol_diff) != 0:
            return 2
        if normalize_map_to_json(after_path, after_json, args.symbol_diff) != 0:
            return 2

        before_label = str(Path(args.before).resolve())
        after_label = str(Path(args.after).resolve())
        report_rc = 0
        if args.report_dir:
            report_dir = DEFAULT_BUILD_DIR / "codesize" / timestamp() if args.report_dir == AUTO_REPORT_DIR else Path(args.report_dir).resolve()
            report_dir.mkdir(parents=True, exist_ok=True)
            base = sanitize_name(f"compare_{comparison_label(before_path)}_vs_{comparison_label(after_path)}")
            markdown_report = report_dir / f"{base}.md"
            json_report = report_dir / f"{base}.json"
            markdown_rc = run_python(
                script_path("compare_codesize.py"),
                compare_command_args(before_json, after_json, args, "markdown", markdown_report, before_label, after_label),
            )
            json_rc = run_python(
                script_path("compare_codesize.py"),
                compare_command_args(before_json, after_json, args, "json", json_report, before_label, after_label),
            )
            report_rc = max(markdown_rc, json_rc)
            if markdown_report.is_file() and json_report.is_file():
                print(f"Reports generated under: {report_dir}")
                print(f"- {markdown_report}")
                print(f"- {json_report}")

        if args.output:
            output_rc = run_python(
                script_path("compare_codesize.py"),
                compare_command_args(
                    before_json,
                    after_json,
                    args,
                    args.format,
                    Path(args.output).resolve(),
                    before_label,
                    after_label,
                ),
            )
            return max(report_rc, output_rc)
        if args.report_dir:
            return report_rc
        return run_python(
            script_path("compare_codesize.py"),
            compare_command_args(before_json, after_json, args, args.format, None, before_label, after_label),
        )


def path_size(path: Path) -> int:
    if path.is_file():
        return path.stat().st_size
    if path.is_dir():
        return sum(item.stat().st_size for item in path.rglob("*") if item.is_file())
    return 0


def resource_image_path(directory: Path) -> Optional[Path]:
    """Return the final generated resource image, excluding staging trees."""
    mmap_build = directory / "mmap_build"
    if not mmap_build.is_dir():
        return None
    candidates = [
        path for path in mmap_build.rglob("res.bin")
        if path.is_file() and ".res.bin.tmp" not in path.parts
    ]
    if not candidates:
        return None
    return max(candidates, key=lambda item: item.stat().st_size)


def parse_partition_size(value: str) -> Optional[int]:
    match = re.fullmatch(r"\s*(0x[0-9a-fA-F]+|\d+)\s*([KkMm]?)\s*", value)
    if not match:
        return None
    number = int(match.group(1), 0)
    suffix = match.group(2).lower()
    if suffix == "k":
        number *= 1024
    elif suffix == "m":
        number *= 1024 * 1024
    return number


def collect_partition_stats(directory: Path) -> List[Dict[str, object]]:
    """Read the project partition table and attach matching image sizes."""
    project_root = directory.parent
    sdkconfig = project_root / "sdkconfig"
    if not sdkconfig.is_file():
        return []
    table_name = None
    for line in sdkconfig.read_text(encoding="utf-8", errors="ignore").splitlines():
        match = re.match(r'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="([^"]+)"', line)
        if match:
            table_name = match.group(1)
            break
    if not table_name:
        return []
    table_path = project_root / table_name
    if not table_path.is_file():
        return []

    image_names = {
        "ota_0": "app_bin",
        "ota_1": "app_bin",
        "factory": "app_bin",
        "res": "resources",
    }
    result: List[Dict[str, object]] = []
    with table_path.open(newline="", encoding="utf-8") as table:
        for row in csv.reader(table):
            if not row or not row[0].strip() or row[0].lstrip().startswith("#"):
                continue
            fields = [field.strip() for field in row]
            if len(fields) < 5:
                continue
            size = parse_partition_size(fields[4])
            offset = parse_partition_size(fields[3])
            if size is None:
                continue
            result.append({
                "name": fields[0],
                "type": fields[1],
                "subtype": fields[2],
                "offset": offset or 0,
                "size": size,
                "image": image_names.get(fields[0]),
            })
    return result


def collect_package_stats(directory: Path) -> Dict[str, int]:
    description = load_project_description(directory)
    app_bin = description.get("app_bin", "microtech.bin")
    app_elf = description.get("app_elf", "microtech.elf")
    app_map = f"{Path(app_elf).stem}.map"
    resource_image = resource_image_path(directory)
    stats = {
        "app_bin": path_size(directory / app_bin),
        "app_elf": path_size(directory / app_elf),
        "app_map": path_size(directory / app_map),
        "bootloader_bin": path_size(directory / "bootloader" / "bootloader.bin"),
        "partition_table_bin": path_size(directory / "partition_table" / "partition-table.bin"),
        "ota_data_bin": path_size(directory / "ota_data_initial.bin"),
        "resources": path_size(resource_image) if resource_image is not None else 0,
        "zip_total": sum(item.stat().st_size for item in directory.glob("*.zip") if item.is_file()),
    }
    return stats


def build_partition_report(directory: Path, sizes: Dict[str, int]) -> List[Dict[str, object]]:
    report = []
    for partition in collect_partition_stats(directory):
        image_name = partition.get("image")
        image_size = sizes.get(image_name, 0) if image_name else 0
        size = int(partition["size"])
        free = max(size - image_size, 0)
        used_pct = (image_size / size * 100.0) if size and image_size else 0.0
        report.append({
            **partition,
            "image_size": image_size,
            "free": free,
            "used_pct": round(used_pct, 2),
        })
    return report


def format_size(size_bytes: int) -> str:
    if abs(size_bytes) < 1024:
        return f"{size_bytes} B"
    if abs(size_bytes) < 1024 * 1024:
        return f"{size_bytes / 1024:.2f} KB"
    return f"{size_bytes / (1024 * 1024):.2f} MB"


def package_report_markdown(directories: List[Path]) -> str:
    metrics = [
        "app_bin",
        "app_elf",
        "app_map",
        "bootloader_bin",
        "partition_table_bin",
        "ota_data_bin",
        "resources",
        "zip_total",
    ]
    all_stats = [collect_package_stats(directory) for directory in directories]
    all_partitions = [
        build_partition_report(directory, stats)
        for directory, stats in zip(directories, all_stats)
    ]
    lines = [f"# {PROJECT_LABEL} Package Size Report", ""]
    if len(directories) == 1:
        lines.append(f"- **Directory**: {directories[0]}")
        lines.append("")
        lines.append("| Metric | Size |")
        lines.append("|--------|------|")
        for metric in metrics:
            lines.append(f"| {metric} | {format_size(all_stats[0].get(metric, 0))} |")
        if all_partitions[0]:
            lines.append("")
            lines.append("## Partition Headroom")
            lines.append("")
            lines.append("| Partition | Image | Used | Size | Free | Used % |")
            lines.append("|-----------|-------|------|------|------|--------|")
            for item in all_partitions[0]:
                if not item.get("image"):
                    continue
                lines.append(
                    f"| {item['name']} | {item['image']} | "
                    f"{format_size(int(item['image_size']))} | {format_size(int(item['size']))} | "
                    f"{format_size(int(item['free']))} | {item['used_pct']:.2f}% |"
                )
    else:
        lines.append(f"- **Baseline**: {directories[0]}")
        lines.append(f"- **Compared**: {directories[1]}")
        lines.append("")
        lines.append("| Metric | Baseline | Compared | Delta |")
        lines.append("|--------|----------|----------|-------|")
        for metric in metrics:
            before = all_stats[0].get(metric, 0)
            after = all_stats[1].get(metric, 0)
            delta = after - before
            sign = "+" if delta > 0 else ""
            lines.append(f"| {metric} | {format_size(before)} | {format_size(after)} | {sign}{format_size(delta)} |")
        if all_partitions[1]:
            lines.append("")
            lines.append("## Partition Headroom (Compared)")
            lines.append("")
            lines.append("| Partition | Image | Used | Size | Free | Used % |")
            lines.append("|-----------|-------|------|------|------|--------|")
            for item in all_partitions[1]:
                if not item.get("image"):
                    continue
                lines.append(
                    f"| {item['name']} | {item['image']} | "
                    f"{format_size(int(item['image_size']))} | {format_size(int(item['size']))} | "
                    f"{format_size(int(item['free']))} | {item['used_pct']:.2f}% |"
                )
    lines.append("")
    return "\n".join(lines)


def package_report_json(directories: List[Path]) -> str:
    versions = []
    for directory in directories:
        sizes = collect_package_stats(directory)
        versions.append({
            "path": str(directory),
            "sizes": sizes,
            "partitions": build_partition_report(directory, sizes),
        })
    data = {
        "schema_version": 2,
        "project": PROJECT_LABEL,
        "target": "microtech (ESP32-S3)",
        "generated_at": datetime.now().isoformat(timespec="seconds"),
        "versions": versions,
    }
    if len(directories) == 2:
        before = data["versions"][0]["sizes"]
        after = data["versions"][1]["sizes"]
        data["delta"] = {key: after.get(key, 0) - before.get(key, 0) for key in sorted(set(before) | set(after))}
    return json.dumps(data, indent=2, ensure_ascii=False)


def package_report_csv(directories: List[Path]) -> str:
    output = StringIO()
    writer = csv.writer(output)
    metrics = [
        "app_bin",
        "app_elf",
        "app_map",
        "bootloader_bin",
        "partition_table_bin",
        "ota_data_bin",
        "resources",
        "zip_total",
    ]
    stats = [collect_package_stats(directory) for directory in directories]
    if len(directories) == 1:
        writer.writerow(["metric", "size"])
        for metric in metrics:
            writer.writerow([metric, stats[0].get(metric, 0)])
    else:
        writer.writerow(["metric", "before", "after", "delta"])
        for metric in metrics:
            before = stats[0].get(metric, 0)
            after = stats[1].get(metric, 0)
            writer.writerow([metric, before, after, after - before])
    return output.getvalue()


def package_size(args: argparse.Namespace) -> int:
    if args.before or args.after:
        if not args.before or not args.after:
            print("Error: both --before and --after are required for package comparison", file=sys.stderr)
            return 2
        directories = [Path(args.before).resolve(), Path(args.after).resolve()]
    else:
        directories = [Path(args.path).resolve() if args.path else DEFAULT_BUILD_DIR]
    for directory in directories:
        if not directory.is_dir():
            print(f"Error: package directory not found - {directory}", file=sys.stderr)
            return 2

    if args.format == "json":
        output = package_report_json(directories)
    elif args.format == "csv":
        output = package_report_csv(directories)
    else:
        output = package_report_markdown(directories)

    if args.output:
        try:
            Path(args.output).write_text(output, encoding="utf-8")
        except OSError as exc:
            print(f"Error: cannot write {args.output}: {exc}", file=sys.stderr)
            return 2
        print(f"Report generated: {args.output}")
    else:
        print(output)
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=f"{PROJECT_LABEL} firmware code size workflow")
    subparsers = parser.add_subparsers(dest="command", required=True)

    current = subparsers.add_parser("analyze-current", help="Analyze the current application map")
    current.add_argument("--build-dir", default=str(DEFAULT_BUILD_DIR))
    current.add_argument("--map")
    current.add_argument("--top", type=int, default=20)
    current.add_argument("--threshold", type=float, default=1.0)
    current.add_argument("--format", choices=["markdown", "json", "csv"], default="markdown")
    current.add_argument("--show-symbols", action="store_true")
    current.add_argument("--group-by", choices=["library", "domain", "object"], default="library")
    current.add_argument("--output")
    current.add_argument("--fail-on-threshold", type=float)
    current.set_defaults(func=analyze_current)

    archives = subparsers.add_parser("analyze-archives", help="Analyze all application archives with the toolchain size command")
    archives.add_argument("--build-dir", default=str(DEFAULT_BUILD_DIR))
    archives.add_argument("--top", type=int, default=20)
    archives.add_argument("--threshold", type=float, default=5.0)
    archives.add_argument("--format", choices=["markdown", "json", "csv"], default="markdown")
    archives.add_argument("--output")
    archives.add_argument("--fail-on-threshold", type=float)
    archives.set_defaults(func=analyze_archives)

    compare = subparsers.add_parser("compare-builds", help="Compare two build directories or map files")
    compare.add_argument("--before", required=True, help="Baseline build directory or map file")
    compare.add_argument("--after", required=True, help="Compared build directory or map file")
    compare.add_argument("--top", type=int, default=20)
    compare.add_argument("--threshold", type=float, default=5.0)
    compare.add_argument("--format", choices=["markdown", "json", "csv"], default="markdown")
    compare.add_argument("--output")
    compare.add_argument("--report-dir", nargs="?", const=AUTO_REPORT_DIR, help="Persist Markdown and JSON reports under this directory or build/codesize/<timestamp>")
    compare.add_argument("--object-diff", action="store_true")
    compare.add_argument("--symbol-diff", action="store_true")
    compare.add_argument("--budget")
    compare.add_argument("--fail-on-budget", action="store_true")
    compare.add_argument("--fail-on-growth", type=float, help="Fail if Flash growth exceeds this percentage")
    compare.set_defaults(func=compare_builds)

    package = subparsers.add_parser("package-size", help="Summarize build package sizes")
    package.add_argument("--path")
    package.add_argument("--before")
    package.add_argument("--after")
    package.add_argument("--format", choices=["markdown", "json", "csv"], default="markdown")
    package.add_argument("--output")
    package.set_defaults(func=package_size)

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
