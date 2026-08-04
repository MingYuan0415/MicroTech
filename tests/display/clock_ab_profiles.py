#!/usr/bin/env python3
"""Prepare and validate isolated ESP32-S3 display clock A/B profiles."""

from __future__ import annotations

import argparse
import hashlib
import ipaddress
import json
import re
import shlex
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


CONFIG_ASSIGNMENT = re.compile(r"^(CONFIG_[A-Z0-9_]+)=(.*)$")
CONFIG_NOT_SET = re.compile(r"^# (CONFIG_[A-Z0-9_]+) is not set$")
CLOCK_CONFIG_KEYS = frozenset(
    {
        "CONFIG_BSP_DISPLAY_SPI_CLOCK_40M",
        "CONFIG_BSP_DISPLAY_SPI_CLOCK_80M",
        "CONFIG_BSP_DISPLAY_SPI_CLOCK_HZ",
    }
)
BENCHMARK_DURATION_KEY = "stress_duration_sec"
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


class ProfileError(RuntimeError):
    """Raised when a clock A/B profile is incomplete or inconsistent."""


@dataclass(frozen=True)
class Profile:
    """One firmware configuration in the display clock A/B matrix."""

    name: str
    variant_defaults: str
    clock_defaults: str
    clock_hz: int
    draw_rows: int
    rgb565_swapped: bool
    benchmark_profile: str


@dataclass(frozen=True)
class ProfilePaths:
    """Generated paths reserved for one isolated IDF build."""

    root: Path
    sdkconfig: Path
    build: Path
    manifest: Path
    header: Path


PROFILES = {
    "E40": Profile("E40", "experimental.defaults", "clock_40.defaults",
                   40_000_000, 120, True, "characterization_full.json"),
    "E80": Profile("E80", "experimental.defaults", "clock_80.defaults",
                   80_000_000, 120, True, "characterization_full.json"),
    "P40": Profile("P40", "production.defaults", "clock_40.defaults",
                   40_000_000, 60, False, "characterization_full.json"),
    "P80": Profile("P80", "production.defaults", "clock_80.defaults",
                   80_000_000, 60, False, "characterization_full.json"),
    "E80-STRESS": Profile(
        "E80-STRESS", "experimental.defaults", "clock_80.defaults",
        80_000_000, 120, True, "stress_1800.json",
    ),
    "E80-SOAK": Profile(
        "E80-SOAK", "experimental.defaults", "clock_80.defaults",
        80_000_000, 120, True, "soak_28800.json",
    ),
}
CHARACTERIZATION_PROFILE_ORDER = ("E40", "E80", "P40", "P80")
LONG_RUN_PROFILE_ORDER = ("E80-STRESS", "E80-SOAK")
PROFILE_ORDER = CHARACTERIZATION_PROFILE_ORDER + LONG_RUN_PROFILE_ORDER
PROFILE_PAIRS = {
    "experimental": ("E40", "E80"),
    "production": ("P40", "P80"),
    "all": CHARACTERIZATION_PROFILE_ORDER,
}

COMMON_EXPECTED = {
    "CONFIG_BSP_DISPLAY_SPI_MAX_TRANSFER_LINES": "10",
    "CONFIG_BSP_DISPLAY_SPI_TRANS_QUEUE_DEPTH": "2",
    "CONFIG_BSP_DISPLAY_NON_TE_PSRAM_DMA_DIRECT": "n",
    "CONFIG_BSP_DISPLAY_TE_SYNC": "n",
    "CONFIG_SYSTEM_PM_DEVELOPMENT_MODE": "y",
    "CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL": "131072",
    "CONFIG_APP_MANAGER_LIFECYCLE_DEBUG_LOG": "n",
    "CONFIG_APP_MANAGER_DISPLAY_DIAGNOSTICS": "y",
    "CONFIG_MAIN_DISPLAY_BENCHMARK": "y",
    "CONFIG_LV_OS_NONE": "n",
    "CONFIG_LV_OS_FREERTOS": "y",
    "CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT": "2",
    "CONFIG_LV_DRAW_THREAD_STACK_SIZE": "32768",
    "CONFIG_ESP_LVGL_ADAPTER_FREETYPE_SMALL_RENDER_POOL": "n",
    "CONFIG_PROVISIONING_SERVICE_DIAGNOSTICS": "n",
}


def project_root() -> Path:
    """Return the repository root containing this utility."""
    return Path(__file__).resolve().parents[2]


def config_assets_dir(root: Path) -> Path:
    """Return the tracked directory containing A/B defaults fragments."""
    return root / "tests" / "display" / "profile_defaults"


def benchmark_profiles_dir(root: Path) -> Path:
    """Return the tracked directory containing strict campaign profiles."""
    return root / "tests" / "display" / "profiles"


def load_benchmark_profile(path: Path) -> dict[str, object]:
    """Load and validate one strict benchmark JSON profile."""
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ProfileError(f"cannot read benchmark profile {path}: {error}") from error
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
        raise ProfileError("benchmark ble_mode must be off or security2_connected")
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
        raise ProfileError("benchmark ipv4_host must be a valid IPv4 address") from error
    return value


def benchmark_profile(root: Path, profile: Profile) -> dict[str, object]:
    """Return the validated campaign selected by one firmware profile."""
    return load_benchmark_profile(
        benchmark_profiles_dir(root) / profile.benchmark_profile
    )


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
        "security2_connected":
            "DISPLAY_BENCHMARK_BLE_SECURITY2_CONNECTED",
    }[str(value["ble_mode"])]
    app_workload = {
        "display_routes":
            "DISPLAY_BENCHMARK_APP_WORKLOAD_DISPLAY_ROUTES",
        "system_routes":
            "DISPLAY_BENCHMARK_APP_WORKLOAD_SYSTEM_ROUTES",
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


def profile_defaults(root: Path, profile: Profile) -> tuple[Path, ...]:
    """Return ordered SDKCONFIG_DEFAULTS files for a profile."""
    assets = config_assets_dir(root)
    defaults = (
        root / "sdkconfig.defaults",
        assets / "clock_ab_common.defaults",
        assets / profile.variant_defaults,
        assets / profile.clock_defaults,
    )
    return defaults


def profile_paths(output_dir: Path, profile: Profile) -> ProfilePaths:
    """Return isolated sdkconfig and build paths for a profile."""
    root = output_dir.resolve() / profile.name.lower()
    return ProfilePaths(
        root,
        root / "sdkconfig",
        root / "build",
        root / "source_manifest.json",
        root / "display_benchmark_profile.h",
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
    """Fingerprint commits and local source contents across all repositories."""
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
    path: Path, profile: Profile, state: dict[str, object]
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


def expected_values(profile: Profile) -> dict[str, str]:
    """Return mandatory values in a materialized profile sdkconfig."""
    values = dict(COMMON_EXPECTED)
    values.update(
        {
            "CONFIG_APP_MANAGER_LVGL_PARTIAL_BUFFER_HEIGHT":
                str(profile.draw_rows),
            "CONFIG_APP_MANAGER_LVGL_RGB565_SWAPPED":
                "y" if profile.rgb565_swapped else "n",
            "CONFIG_BSP_DISPLAY_SPI_CLOCK_40M":
                "y" if profile.clock_hz == 40_000_000 else "n",
            "CONFIG_BSP_DISPLAY_SPI_CLOCK_80M":
                "y" if profile.clock_hz == 80_000_000 else "n",
            "CONFIG_BSP_DISPLAY_SPI_CLOCK_HZ": str(profile.clock_hz),
        }
    )
    return values


def benchmark_duration_range(root: Path) -> tuple[int, int]:
    """Return the runtime validation range shared with display_benchmark.c."""
    del root
    return 10, 28800


def _assert_expected(name: str, values: dict[str, str],
                     expected: dict[str, str]) -> None:
    mismatches = []
    for key, expected_value in expected.items():
        actual_value = values.get(key, "<missing>")
        if actual_value != expected_value:
            mismatches.append(f"{key}={actual_value} (expected {expected_value})")
    if mismatches:
        raise ProfileError(f"{name} has invalid settings: " + ", ".join(mismatches))


def validate_assets(root: Path) -> None:
    """Validate tracked fragments and their profile composition."""
    assets = config_assets_dir(root)
    common = parse_config(assets / "clock_ab_common.defaults")
    _assert_expected("common defaults", common, COMMON_EXPECTED)
    if set(common) != set(COMMON_EXPECTED):
        extras = sorted(set(common) - set(COMMON_EXPECTED))
        raise ProfileError(f"common defaults contain unexpected settings: {extras}")

    expected_fragments = {
        "experimental.defaults": {
            "CONFIG_APP_MANAGER_LVGL_PARTIAL_BUFFER_HEIGHT": "120",
            "CONFIG_APP_MANAGER_LVGL_RGB565_SWAPPED": "y",
        },
        "production.defaults": {
            "CONFIG_APP_MANAGER_LVGL_PARTIAL_BUFFER_HEIGHT": "60",
            "CONFIG_APP_MANAGER_LVGL_RGB565_SWAPPED": "n",
        },
        "clock_40.defaults": {
            "CONFIG_BSP_DISPLAY_SPI_CLOCK_40M": "y",
            "CONFIG_BSP_DISPLAY_SPI_CLOCK_80M": "n",
        },
        "clock_80.defaults": {
            "CONFIG_BSP_DISPLAY_SPI_CLOCK_40M": "n",
            "CONFIG_BSP_DISPLAY_SPI_CLOCK_80M": "y",
        },
    }
    for filename, expected_fragment in expected_fragments.items():
        fragment = parse_config(assets / filename)
        if fragment != expected_fragment:
            raise ProfileError(
                f"{filename} has unexpected settings: {fragment}"
            )

    loaded_profiles = {
        name: benchmark_profile(root, PROFILES[name]) for name in PROFILE_ORDER
    }
    for name in CHARACTERIZATION_PROFILE_ORDER:
        if loaded_profiles[name]["mode"] != "characterization":
            raise ProfileError(f"{name} must use characterization mode")
    for name in LONG_RUN_PROFILE_ORDER:
        if loaded_profiles[name]["mode"] != "stress":
            raise ProfileError(f"{name} must use stress mode")

    for name in PROFILE_ORDER:
        profile = PROFILES[name]
        merged = merge_configs(profile_defaults(root, profile))
        expected = expected_values(profile)
        expected.pop("CONFIG_BSP_DISPLAY_SPI_CLOCK_HZ")
        _assert_expected(f"{name} defaults", merged, expected)

    for first_name, second_name in (("E40", "E80"), ("P40", "P80")):
        first = merge_configs(profile_defaults(root, PROFILES[first_name]))
        second = merge_configs(profile_defaults(root, PROFILES[second_name]))
        differences = differing_keys(first, second)
        expected_differences = CLOCK_CONFIG_KEYS - {
            "CONFIG_BSP_DISPLAY_SPI_CLOCK_HZ"
        }
        if differences != expected_differences:
            raise ProfileError(
                f"{first_name}/{second_name} defaults differ outside clock "
                f"choice: {sorted(differences)}"
            )

    stress = loaded_profiles["E80-STRESS"]
    soak = loaded_profiles["E80-SOAK"]
    differences = differing_keys(stress, soak)
    if differences != {BENCHMARK_DURATION_KEY}:
        raise ProfileError(
            "E80-STRESS/E80-SOAK profiles must differ only by duration: "
            f"{sorted(differences)}"
        )


def differing_keys(first: dict[str, object],
                   second: dict[str, object]) -> set[str]:
    """Return symbols with unequal values, including symbols absent on one side."""
    return {
        key for key in first.keys() | second.keys()
        if first.get(key) != second.get(key)
    }


def build_command(root: Path, output_dir: Path, profile: Profile) -> list[str]:
    """Return the IDF build command for one isolated profile."""
    paths = profile_paths(output_dir, profile)
    defaults = ";".join(str(path) for path in profile_defaults(root, profile))
    return [
        "idf.py",
        "-B",
        str(paths.build),
        f"-DSDKCONFIG={paths.sdkconfig}",
        f"-DSDKCONFIG_DEFAULTS={defaults}",
        f"-DDISPLAY_BENCHMARK_PROFILE_DIR={paths.root}",
        "-DIDF_TARGET=esp32s3",
        "build",
        "size",
    ]


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


def prepare(root: Path, output_dir: Path, reset: bool,
            profile_names: Iterable[str] =
            CHARACTERIZATION_PROFILE_ORDER) -> list[str]:
    """Create isolated profile directories and return their build commands."""
    validate_assets(root)
    _ensure_isolated(root, output_dir)
    state = source_manifest(root)
    commands = []
    for name in profile_names:
        profile = PROFILES[name]
        paths = profile_paths(output_dir, profile)
        if reset:
            if paths.build.is_symlink():
                paths.build.unlink()
            elif paths.build.exists():
                shutil.rmtree(paths.build)
            for path in (
                    paths.sdkconfig,
                    paths.sdkconfig.with_suffix(".old"),
                    paths.manifest,
                    paths.header):
                if path.exists() or path.is_symlink():
                    path.unlink()
        paths.root.mkdir(parents=True, exist_ok=True)
        if paths.manifest.exists():
            existing = _read_profile_manifest(paths.manifest)
            expected = dict(state)
            expected["profile"] = profile.name
            if existing != expected:
                raise ProfileError(
                    f"{profile.name} source state changed; rerun prepare "
                    "with --reset"
                )
        else:
            if paths.sdkconfig.exists() or paths.build.exists():
                raise ProfileError(
                    f"{profile.name} has unstamped build artifacts; rerun "
                    "prepare with --reset"
                )
            _write_profile_manifest(paths.manifest, profile, state)
        expected_header = render_profile_header(benchmark_profile(root, profile))
        if paths.header.exists():
            if paths.header.read_text(encoding="utf-8") != expected_header:
                raise ProfileError(
                    f"{profile.name} generated profile changed; rerun prepare "
                    "with --reset"
                )
        else:
            paths.header.write_text(expected_header, encoding="utf-8")
        if paths.sdkconfig.exists():
            validate_materialized_profile(paths.sdkconfig, profile)
        commands.append(shlex.join(build_command(root, output_dir, profile)))
    return commands


def validate_materialized_profile(path: Path, profile: Profile) -> dict[str, str]:
    """Validate one sdkconfig created by ESP-IDF and return all its values."""
    values = parse_config(path)
    _assert_expected(profile.name, values, expected_values(profile))
    return values


def validate_profile_artifacts(
    root: Path, output_dir: Path, profile: Profile,
    state: dict[str, object] | None = None,
) -> dict[str, str]:
    """Validate one profile config, firmware image, and source manifest."""
    paths = profile_paths(output_dir, profile)
    values = validate_materialized_profile(paths.sdkconfig, profile)
    expected_header = render_profile_header(benchmark_profile(root, profile))
    try:
        actual_header = paths.header.read_text(encoding="utf-8")
    except OSError as error:
        raise ProfileError(
            f"{profile.name} generated profile header is missing"
        ) from error
    if actual_header != expected_header:
        raise ProfileError(
            f"{profile.name} generated profile header does not match JSON"
        )
    if not (paths.build / "microtech.bin").is_file():
        raise ProfileError(f"{profile.name} firmware image is missing")
    expected = dict(state if state is not None else source_manifest(root))
    expected["profile"] = profile.name
    actual = _read_profile_manifest(paths.manifest)
    if actual != expected:
        raise ProfileError(
            f"{profile.name} source manifest does not match the current "
            "repository state"
        )
    return values


def validate_pair(output_dir: Path, first_name: str,
                  second_name: str, root: Path | None = None) -> set[str]:
    """Validate one clock pair and require an exact clock-only full diff."""
    root = project_root() if root is None else root
    state = source_manifest(root)
    first_profile = PROFILES[first_name]
    second_profile = PROFILES[second_name]
    first = validate_profile_artifacts(
        root, output_dir, first_profile, state
    )
    second = validate_profile_artifacts(
        root, output_dir, second_profile, state
    )
    if benchmark_profile(root, first_profile) != benchmark_profile(
            root, second_profile):
        raise ProfileError(
            f"{first_name}/{second_name} use different benchmark profiles"
        )
    differences = differing_keys(first, second)
    if differences != CLOCK_CONFIG_KEYS:
        unexpected = differences - CLOCK_CONFIG_KEYS
        missing = CLOCK_CONFIG_KEYS - differences
        raise ProfileError(
            f"{first_name}/{second_name} are not a clock-only pair: "
            f"unexpected={sorted(unexpected)} missing={sorted(missing)}"
        )
    return differences


def validate_long_run_pair(
    output_dir: Path, root: Path | None = None
) -> set[str]:
    """Require stress and soak sdkconfigs to differ only by run duration."""
    root = project_root() if root is None else root
    state = source_manifest(root)
    stress_profile = PROFILES["E80-STRESS"]
    soak_profile = PROFILES["E80-SOAK"]
    stress_values = validate_profile_artifacts(
        root, output_dir, stress_profile, state
    )
    soak_values = validate_profile_artifacts(
        root, output_dir, soak_profile, state
    )
    sdkconfig_differences = differing_keys(stress_values, soak_values)
    if sdkconfig_differences:
        raise ProfileError(
            "E80-STRESS/E80-SOAK sdkconfigs must be identical: "
            f"differences={sorted(sdkconfig_differences)}"
        )
    stress = benchmark_profile(root, stress_profile)
    soak = benchmark_profile(root, soak_profile)
    differences = differing_keys(stress, soak)
    if differences != {BENCHMARK_DURATION_KEY}:
        raise ProfileError(
            "E80-STRESS/E80-SOAK are not a duration-only pair: "
            f"differences={sorted(differences)}"
        )
    return differences


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Prepare and validate ESP32-S3 display clock A/B profiles"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    prepare_parser = subparsers.add_parser(
        "prepare", help="create isolated directories and print build commands"
    )
    prepare_parser.add_argument(
        "--output-dir", type=Path, default=Path("/tmp/mt-display-clock-ab")
    )
    prepare_parser.add_argument(
        "--reset", action="store_true",
        help="remove generated sdkconfig and build artifacts first",
    )
    prepare_parser.add_argument(
        "--profiles", nargs="+", choices=PROFILE_ORDER,
        default=CHARACTERIZATION_PROFILE_ORDER,
        help="profiles to prepare (default: four characterization profiles)",
    )

    command_parser = subparsers.add_parser(
        "command", help="print the build command for one profile"
    )
    command_parser.add_argument("profile", choices=PROFILE_ORDER)
    command_parser.add_argument(
        "--output-dir", type=Path, default=Path("/tmp/mt-display-clock-ab")
    )

    validate_parser = subparsers.add_parser(
        "validate", help="validate materialized sdkconfigs and pairwise diffs"
    )
    validate_parser.add_argument(
        "--output-dir", type=Path, default=Path("/tmp/mt-display-clock-ab")
    )
    validation_group = validate_parser.add_mutually_exclusive_group()
    validation_group.add_argument(
        "--pair", choices=PROFILE_PAIRS, default="all"
    )
    validation_group.add_argument(
        "--profiles", nargs="+", choices=PROFILE_ORDER,
        help="validate one or more materialized profiles",
    )
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    root = project_root()
    try:
        if args.command == "prepare":
            for command in prepare(
                    root, args.output_dir, args.reset, args.profiles):
                print(command)
        elif args.command == "command":
            validate_assets(root)
            _ensure_isolated(root, args.output_dir)
            print(shlex.join(build_command(
                root, args.output_dir, PROFILES[args.profile]
            )))
        else:
            validate_assets(root)
            _ensure_isolated(root, args.output_dir)
            if args.profiles is not None:
                state = source_manifest(root)
                for name in args.profiles:
                    profile = PROFILES[name]
                    validate_profile_artifacts(
                        root, args.output_dir, profile, state
                    )
                    print(f"{name}: PASS")
                if set(LONG_RUN_PROFILE_ORDER).issubset(args.profiles):
                    differences = validate_long_run_pair(args.output_dir, root)
                    print(
                        "E80-STRESS/E80-SOAK: PASS duration-only "
                        f"differences={','.join(sorted(differences))}"
                    )
                return 0
            selected = PROFILE_PAIRS[args.pair]
            pairs = []
            if "E40" in selected:
                pairs.append(("E40", "E80"))
            if "P40" in selected:
                pairs.append(("P40", "P80"))
            for first, second in pairs:
                differences = validate_pair(
                    args.output_dir, first, second, root
                )
                print(
                    f"{first}/{second}: PASS clock-only "
                    f"differences={','.join(sorted(differences))}"
                )
    except ProfileError as error:
        print(f"error: {error}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
