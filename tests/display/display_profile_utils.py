#!/usr/bin/env python3
"""Shared helpers for isolated display benchmark firmware profiles."""

from __future__ import annotations

import hashlib
import ipaddress
import json
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Protocol


CONFIG_ASSIGNMENT = re.compile(r"^(CONFIG_[A-Z0-9_]+)=(.*)$")
CONFIG_NOT_SET = re.compile(r"^# (CONFIG_[A-Z0-9_]+) is not set$")
BENCHMARK_PROFILE_FIELDS = frozenset(
    {
        "mode",
        "stress_duration_sec",
        "effect_duration_sec",
        "load",
        "ble_mode",
        "app_workload",
        "audio_volume_percent",
        "ipv4_host",
        "port",
        "rate_kbit_s",
    }
)
SOURCE_MANIFEST_VERSION = 1
COMMON_EXPECTED = {
    "CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL": "131072",
    "CONFIG_APP_MANAGER_LIFECYCLE_DEBUG_LOG": "n",
    "CONFIG_APP_MANAGER_DISPLAY_DIAGNOSTICS": "y",
    "CONFIG_APP_MANAGER_LVGL_PARTIAL_BUFFER_HEIGHT": "60",
    "CONFIG_APP_MANAGER_PRESENTATION_SNAPSHOT_ANIMATION": "y",
    "CONFIG_MAIN_DISPLAY_BENCHMARK": "y",
    "CONFIG_LV_OS_NONE": "n",
    "CONFIG_LV_OS_FREERTOS": "y",
    "CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT": "2",
    "CONFIG_LV_DRAW_THREAD_STACK_SIZE": "32768",
    "CONFIG_ESP_LVGL_ADAPTER_FREETYPE_SMALL_RENDER_POOL": "n",
    "CONFIG_PROVISIONING_SERVICE_DIAGNOSTICS": "n",
}


class ProfileError(RuntimeError):
    """Raised when an isolated display profile is inconsistent."""


class NamedProfile(Protocol):
    """Minimal profile identity stored in a source manifest."""

    name: str


@dataclass(frozen=True)
class ProfilePaths:
    """Generated paths reserved for one isolated IDF build."""

    root: Path
    sdkconfig: Path
    build: Path
    manifest: Path
    header: Path


def project_root() -> Path:
    """Return the repository root containing this utility."""
    return Path(__file__).resolve().parents[2]


def config_assets_dir(root: Path) -> Path:
    """Return the tracked directory containing defaults fragments."""
    return root / "tests" / "display" / "profile_defaults"


def benchmark_profiles_dir(root: Path) -> Path:
    """Return the tracked directory containing strict campaign profiles."""
    return root / "tests" / "display" / "profiles"


def load_benchmark_profile(path: Path) -> dict[str, object]:
    """Load and validate one strict benchmark JSON profile."""
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ProfileError(
            f"cannot read benchmark profile {path}: {error}"
        ) from error
    if not isinstance(value, dict):
        raise ProfileError(f"benchmark profile must be a JSON object: {path}")
    fields = set(value)
    if fields != BENCHMARK_PROFILE_FIELDS:
        missing = sorted(BENCHMARK_PROFILE_FIELDS - fields)
        unknown = sorted(fields - BENCHMARK_PROFILE_FIELDS)
        raise ProfileError(
            f"benchmark profile schema mismatch: missing={missing} "
            f"unknown={unknown}"
        )
    if value["mode"] not in ("stress", "characterization"):
        raise ProfileError("benchmark mode must be stress or characterization")
    if value["load"] not in ("full", "audio_only", "tcp_only"):
        raise ProfileError("benchmark load must be full, audio_only, or tcp_only")
    if value["ble_mode"] not in ("off", "security2_connected"):
        raise ProfileError(
            "benchmark ble_mode must be off or security2_connected"
        )
    if value["app_workload"] not in ("display_routes", "system_routes"):
        raise ProfileError(
            "benchmark app_workload must be display_routes or system_routes"
        )
    integer_ranges = {
        "stress_duration_sec": (10, 28800),
        "effect_duration_sec": (5, 300),
        "port": (1, 65535),
        "rate_kbit_s": (64, 20000),
        "audio_volume_percent": (0, 100),
    }
    for field, (minimum, maximum) in integer_ranges.items():
        field_value = value[field]
        if type(field_value) is not int or not minimum <= field_value <= maximum:
            raise ProfileError(
                f"benchmark {field} must be an integer in {minimum}..{maximum}"
            )
    if not isinstance(value["ipv4_host"], str) or not value["ipv4_host"]:
        raise ProfileError("benchmark ipv4_host must be a non-empty string")
    try:
        ipaddress.IPv4Address(value["ipv4_host"])
    except ipaddress.AddressValueError as error:
        raise ProfileError(
            "benchmark ipv4_host must be a valid IPv4 address"
        ) from error
    return value


def render_profile_header(value: dict[str, object]) -> str:
    """Render one generated, typed benchmark profile header."""
    mode = {
        "stress": "DISPLAY_BENCHMARK_MODE_STRESS",
        "characterization": "DISPLAY_BENCHMARK_MODE_CHARACTERIZATION",
    }[str(value["mode"])]
    load = {
        "full": "DISPLAY_BENCHMARK_LOAD_FULL",
        "audio_only": "DISPLAY_BENCHMARK_LOAD_AUDIO_ONLY",
        "tcp_only": "DISPLAY_BENCHMARK_LOAD_TCP_ONLY",
    }[str(value["load"])]
    ble_mode = {
        "off": "DISPLAY_BENCHMARK_BLE_OFF",
        "security2_connected": "DISPLAY_BENCHMARK_BLE_SECURITY2_CONNECTED",
    }[str(value["ble_mode"])]
    app_workload = {
        "display_routes": "DISPLAY_BENCHMARK_APP_WORKLOAD_DISPLAY_ROUTES",
        "system_routes": "DISPLAY_BENCHMARK_APP_WORKLOAD_SYSTEM_ROUTES",
    }[str(value["app_workload"])]
    return (
        "#ifndef __GENERATED_DISPLAY_BENCHMARK_PROFILE_H__\n"
        "#define __GENERATED_DISPLAY_BENCHMARK_PROFILE_H__\n\n"
        "static const display_benchmark_config_t g_display_benchmark_profile =\n"
        "{\n"
        f"    .mode = {mode},\n"
        f"    .stress_duration_sec = {value['stress_duration_sec']}U,\n"
        f"    .effect_duration_sec = {value['effect_duration_sec']}U,\n"
        f"    .load = {load},\n"
        f"    .ble_mode = {ble_mode},\n"
        f"    .app_workload = {app_workload},\n"
        f"    .audio_volume_percent = {value['audio_volume_percent']}U,\n"
        f"    .ipv4_host = \"{value['ipv4_host']}\",\n"
        f"    .port = {value['port']}U,\n"
        f"    .rate_kbit_s = {value['rate_kbit_s']}U,\n"
        "};\n\n"
        "#endif /* __GENERATED_DISPLAY_BENCHMARK_PROFILE_H__ */\n"
    )


def parse_config(path: Path) -> dict[str, str]:
    """Parse assignments and disabled symbols from an sdkconfig-style file."""
    if not path.is_file():
        raise ProfileError(f"configuration file does not exist: {path}")

    values: dict[str, str] = {}
    for line_number, raw_line in enumerate(
            path.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw_line.strip()
        match = CONFIG_ASSIGNMENT.match(line)
        if match is not None:
            key, value = match.groups()
        else:
            match = CONFIG_NOT_SET.match(line)
            if match is None:
                continue
            key, value = match.group(1), "n"
        if key in values and values[key] != value:
            raise ProfileError(
                f"conflicting {key} definitions in {path}:{line_number}"
            )
        values[key] = value
    return values


def merge_configs(paths: Iterable[Path]) -> dict[str, str]:
    """Merge defaults in ESP-IDF order, where later files override earlier."""
    values: dict[str, str] = {}
    for path in paths:
        values.update(parse_config(path))
    return values


def differing_keys(first: dict[str, object],
                   second: dict[str, object]) -> set[str]:
    """Return keys with unequal values, including absent keys."""
    return {
        key for key in first.keys() | second.keys()
        if first.get(key) != second.get(key)
    }


def validate_assets(root: Path) -> None:
    """Validate the fixed benchmark defaults fragment."""
    common = parse_config(
        config_assets_dir(root) / "benchmark_baseline.defaults"
    )
    if common != COMMON_EXPECTED:
        raise ProfileError(
            f"benchmark_baseline.defaults has unexpected settings: {common}"
        )


def _git_output(repository: Path, *arguments: str) -> bytes:
    result = subprocess.run(
        ("git", "-C", str(repository), *arguments),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        error = result.stderr.decode("utf-8", errors="replace").strip()
        raise ProfileError(
            f"git {' '.join(arguments)} failed in {repository}: {error}"
        )
    return result.stdout


def _repository_paths(root: Path) -> list[Path]:
    repositories = [root.resolve()]
    output = _git_output(root, "submodule", "status", "--recursive")
    for raw_line in output.decode("utf-8", errors="strict").splitlines():
        fields = raw_line.lstrip(" +-U").split()
        if len(fields) < 2:
            raise ProfileError(f"malformed git submodule status: {raw_line!r}")
        repositories.append((root / fields[1]).resolve())
    return repositories


def _repository_state(repository: Path, label: str) -> dict[str, object]:
    commit = _git_output(repository, "rev-parse", "HEAD").decode(
        "ascii", errors="strict"
    ).strip()
    diff = _git_output(
        repository, "diff", "--binary", "--no-color", "--no-ext-diff",
        "HEAD", "--",
    )
    untracked_output = _git_output(
        repository, "ls-files", "--others", "--exclude-standard", "-z"
    )
    untracked = sorted(
        item for item in untracked_output.decode(
            "utf-8", errors="surrogateescape"
        ).split("\0") if item
    )

    digest = hashlib.sha256()
    digest.update(label.encode("utf-8"))
    digest.update(b"\0commit\0")
    digest.update(commit.encode("ascii"))
    digest.update(b"\0diff\0")
    digest.update(diff)
    for relative_name in untracked:
        path = repository / relative_name
        digest.update(b"\0untracked\0")
        digest.update(relative_name.encode("utf-8", errors="surrogateescape"))
        digest.update(b"\0")
        if path.is_symlink():
            digest.update(b"symlink\0")
            digest.update(path.readlink().as_posix().encode("utf-8"))
        elif path.is_file():
            digest.update(b"file\0")
            with path.open("rb") as source:
                for chunk in iter(lambda: source.read(64 * 1024), b""):
                    digest.update(chunk)
        else:
            raise ProfileError(f"unsupported untracked path: {path}")
    return {
        "commit": commit,
        "dirty": bool(diff or untracked),
        "fingerprint": digest.hexdigest(),
        "untracked_count": len(untracked),
    }


def source_manifest(root: Path) -> dict[str, object]:
    """Fingerprint commits and local source contents across repositories."""
    root = root.resolve()
    repositories = {}
    combined = hashlib.sha256()
    for repository in _repository_paths(root):
        relative = repository.relative_to(root)
        label = "." if relative == Path(".") else relative.as_posix()
        state = _repository_state(repository, label)
        repositories[label] = state
        combined.update(label.encode("utf-8"))
        combined.update(b"\0")
        combined.update(str(state["fingerprint"]).encode("ascii"))
        combined.update(b"\0")
    return {
        "version": SOURCE_MANIFEST_VERSION,
        "source_fingerprint": combined.hexdigest(),
        "repositories": repositories,
    }


def _write_profile_manifest(
    path: Path, profile: NamedProfile, state: dict[str, object]
) -> None:
    payload = dict(state)
    payload["profile"] = profile.name
    path.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def _read_profile_manifest(path: Path) -> dict[str, object]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ProfileError(f"cannot read source manifest {path}: {error}") from error
    if not isinstance(payload, dict):
        raise ProfileError(f"invalid source manifest object: {path}")
    return payload


def _ensure_isolated(root: Path, output_dir: Path) -> None:
    root = root.resolve()
    output_dir = output_dir.resolve()
    try:
        output_dir.relative_to(root)
    except ValueError:
        return
    raise ProfileError(
        f"output directory must be outside the repository: {output_dir}"
    )
