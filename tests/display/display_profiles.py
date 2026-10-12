#!/usr/bin/env python3
"""Prepare isolated benchmark campaigns on the production C_EXT configuration."""

from __future__ import annotations

import argparse
import shlex
import shutil
from dataclasses import dataclass
from pathlib import Path

import display_profile_utils as utils


@dataclass(frozen=True)
class Campaign:
    """A runtime workload, using the same firmware configuration."""

    name: str
    filename: str


CAMPAIGNS = {
    "characterization": Campaign("characterization", "characterization_full.json"),
    "stress": Campaign("stress", "stress_1800.json"),
    "ble-stress": Campaign("ble-stress", "c_ext_stress_1800.json"),
    "soak": Campaign("soak", "soak_28800.json"),
}
PRODUCTION_EXPECTED = {
    "CONFIG_LV_OS_NONE": "y",
    "CONFIG_LV_OS_FREERTOS": "n",
    "CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT": "1",
    "CONFIG_APP_MANAGER_LVGL_WORKER_STACK_SIZE": "32768",
    "CONFIG_APP_MANAGER_LVGL_WORKER_AFFINITY_CPU1": "y",
    "CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU0": "y",
    "CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL": "y",
    "CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL": "n",
    "CONFIG_ESP_LVGL_ADAPTER_FREETYPE_SMALL_RENDER_POOL": "n",
}


def profile_paths(output: Path, campaign: Campaign) -> utils.ProfilePaths:
    root = output.resolve() / campaign.name
    return utils.ProfilePaths(root, root / "sdkconfig", root / "build",
                              root / "source_manifest.json",
                              root / "display_benchmark_profile.h")


def _root_defaults(root: Path) -> Path:
    return root / "sdkconfig.defaults"


def benchmark_profile(root: Path, campaign: Campaign) -> dict[str, object]:
    return utils.load_benchmark_profile(
        utils.benchmark_profiles_dir(root) / campaign.filename)


def _campaign_defaults(root: Path, campaign: Campaign, output: Path) -> Path:
    profile = benchmark_profile(root, campaign)
    path = output / campaign.name / "campaign.defaults"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        "CONFIG_MAIN_DISPLAY_BENCHMARK=y\n"
        "CONFIG_APP_MANAGER_DISPLAY_DIAGNOSTICS=y\n"
        "CONFIG_APP_MANAGER_PRESENTATION_SNAPSHOT_ANIMATION=y\n",
        encoding="utf-8")
    return path


def validate_config(values: dict[str, str]) -> None:
    """Check active configuration values; inactive Kconfig booleans may be absent."""
    failures = []
    for key, expected in (utils.COMMON_EXPECTED | PRODUCTION_EXPECTED).items():
        actual = values.get(key, "n" if expected == "n" else "<missing>")
        if actual != expected:
            failures.append(f"{key}={actual} (expected {expected})")
    if values.get("CONFIG_LV_DRAW_THREAD_STACK_SIZE", "n") != "n":
        failures.append("draw thread stack must be inactive for LV_OS_NONE")
    if failures:
        raise utils.ProfileError("invalid settings: " + ", ".join(failures))


def defaults_paths(root: Path) -> tuple[Path, ...]:
    return (utils.config_assets_dir(root) / "benchmark_baseline.defaults",)


def build_command(root: Path, output: Path, campaign: Campaign) -> list[str]:
    paths = profile_paths(output, campaign)
    return ["idf.py", "-B", str(paths.build), f"-DSDKCONFIG={paths.sdkconfig}",
            "-DSDKCONFIG_DEFAULTS=" + ";".join((str(root / "sdkconfig.defaults"),
                                                  str(paths.root / "sdkconfig.defaults"),
                                                  str(utils.config_assets_dir(root) /
                                                      "benchmark_baseline.defaults"))),
            f"-DDISPLAY_BENCHMARK_PROFILE_DIR={paths.root}",
            "-DIDF_TARGET=esp32s3", "build"]


def prepare(root: Path, output: Path, names: list[str], reset: bool) -> list[str]:
    utils._ensure_isolated(root, output)
    utils.validate_assets(root)
    base_values = utils.merge_configs((root / "sdkconfig.defaults",))
    validate_config(base_values | {
        "CONFIG_APP_MANAGER_DISPLAY_DIAGNOSTICS": "y",
        "CONFIG_APP_MANAGER_LVGL_PARTIAL_BUFFER_HEIGHT": "60",
        "CONFIG_MAIN_DISPLAY_BENCHMARK": "y",
        "CONFIG_APP_MANAGER_PRESENTATION_SNAPSHOT_ANIMATION": "y",
        "CONFIG_LV_OS_NONE": "y",
        "CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT": "1",
        "CONFIG_APP_MANAGER_LVGL_WORKER_STACK_SIZE": "32768",
        "CONFIG_APP_MANAGER_LVGL_WORKER_AFFINITY_CPU1": "y",
    })
    state = utils.source_manifest(root)
    commands = []
    for name in names:
        campaign = CAMPAIGNS[name]
        paths = profile_paths(output, campaign)
        if reset and paths.root.exists():
            shutil.rmtree(paths.root)
        paths.root.mkdir(parents=True, exist_ok=True)
        expected_manifest = state | {"profile": name}
        header = utils.render_profile_header(benchmark_profile(root, campaign))
        if paths.manifest.exists():
            if utils._read_profile_manifest(paths.manifest) != expected_manifest:
                raise utils.ProfileError(f"{name}: sources changed; prepare --reset")
        elif paths.build.exists() or paths.sdkconfig.exists():
            raise utils.ProfileError(f"{name}: unstamped artifacts; prepare --reset")
        if paths.header.exists() and paths.header.read_text() != header:
            raise utils.ProfileError(f"{name}: workload changed; prepare --reset")
        campaign_defaults = _campaign_defaults(root, campaign, output)
        defaults = paths.root / "sdkconfig.defaults"
        defaults.write_text(
            "CONFIG_LV_OS_NONE=y\n"
            "CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y\n"
            "# CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL is not set\n"
            "CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=1\n"
            "CONFIG_APP_MANAGER_LVGL_WORKER_STACK_SIZE=32768\n"
            "CONFIG_APP_MANAGER_LVGL_WORKER_AFFINITY_CPU1=y\n"
            "CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU0=y\n"
            + campaign_defaults.read_text(encoding="utf-8"),
            encoding="utf-8")
        utils._write_profile_manifest(paths.manifest, campaign, state)
        paths.header.write_text(header, encoding="utf-8")
        commands.append(shlex.join(build_command(root, output, campaign)))
    return commands


def validate(root: Path, output: Path, names: list[str]) -> None:
    utils._ensure_isolated(root, output)
    state = utils.source_manifest(root)
    configs = []
    for name in names:
        campaign = CAMPAIGNS[name]
        paths = profile_paths(output, campaign)
        values = utils.parse_config(paths.sdkconfig)
        validate_config(values)
        expected_defaults = paths.root / "sdkconfig.defaults"
        if not expected_defaults.is_file():
            raise utils.ProfileError(f"{name}: generated defaults missing")
        if not (paths.build / "microtech.bin").is_file():
            raise utils.ProfileError(f"{name}: firmware image missing")
        if utils._read_profile_manifest(paths.manifest) != state | {"profile": name}:
            raise utils.ProfileError(f"{name}: source manifest mismatch")
        expected = utils.render_profile_header(benchmark_profile(root, campaign))
        if not paths.header.is_file() or paths.header.read_text() != expected:
            raise utils.ProfileError(f"{name}: generated workload mismatch")
        configs.append(values)
    for values in configs[1:]:
        differences = utils.differing_keys(configs[0], values)
        if differences:
            raise utils.ProfileError(f"campaign configuration drift: {sorted(differences)}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare", "validate"))
    parser.add_argument("--campaigns", nargs="+", choices=CAMPAIGNS,
                        default=["characterization"])
    parser.add_argument("--output-dir", type=Path,
                        default=Path("/tmp/mt-display"))
    parser.add_argument("--reset", action="store_true")
    args = parser.parse_args()
    try:
        root = utils.project_root()
        if args.command == "prepare":
            for command in prepare(root, args.output_dir, args.campaigns, args.reset):
                print(command)
        else:
            validate(root, args.output_dir, args.campaigns)
            print("campaign artifacts: PASS")
    except (utils.ProfileError, OSError) as error:
        print(f"error: {error}")
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
