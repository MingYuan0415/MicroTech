#!/usr/bin/env python3
"""Prepare and validate isolated LVGL internal-RAM firmware profiles."""

from __future__ import annotations

import argparse
import shlex
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

import display_profile_utils as display_profiles


ProfileError = display_profiles.ProfileError


@dataclass(frozen=True)
class RamProfile:
    """One LVGL rendering configuration in the internal-RAM matrix."""

    name: str
    defaults: str
    os_none: bool
    draw_units: int
    draw_stack_size: int | None
    small_freetype_pool: bool
    theoretical_reclaim: int
    nimble_external: bool = False
    provisioning_diagnostics: bool = False
    benchmark_profile: str = "characterization_full.json"
    task_affinity: bool = False


PROFILES = {
    "B0": RamProfile("B0", "lvgl_ram_b0.defaults", False, 2, 32768,
                     False, 0),
    "C": RamProfile("C", "lvgl_ram_c.defaults", True, 1, None,
                    False, 65536),
    "A": RamProfile("A", "lvgl_ram_a.defaults", False, 1, 32768,
                    False, 32768),
    "B24": RamProfile("B24", "lvgl_ram_b24.defaults", False, 1, 24576,
                      True, 40960),
    "B20": RamProfile("B20", "lvgl_ram_b20.defaults", False, 1, 20480,
                      True, 45056),
    "B16": RamProfile("B16", "lvgl_ram_b16.defaults", False, 1, 16384,
                      True, 49152),
}
DIAGNOSTIC_PROFILES = {
    "C_EXT": RamProfile(
        "C_EXT", "lvgl_ram_c.defaults", True, 1, None,
        False, 65536, nimble_external=True, task_affinity=True,
    ),
    "C_EXT_STRESS": RamProfile(
        "C_EXT_STRESS", "lvgl_ram_c.defaults", True, 1, None,
        False, 65536, nimble_external=True,
        provisioning_diagnostics=True,
        benchmark_profile="c_ext_stress_1800.json", task_affinity=True,
    ),
}
PRIMARY_PROFILE_ORDER = ("B0", "C")
FALLBACK_PROFILE_ORDER = ("A", "B24", "B20", "B16")
PROFILE_ORDER = PRIMARY_PROFILE_ORDER + FALLBACK_PROFILE_ORDER
DIAGNOSTIC_PROFILE_ORDER = tuple(DIAGNOSTIC_PROFILES)
ALL_PROFILES = PROFILES | DIAGNOSTIC_PROFILES
ALL_PROFILE_ORDER = PROFILE_ORDER + DIAGNOSTIC_PROFILE_ORDER
RAM_CONFIG_KEYS = frozenset(
    {
        "CONFIG_LV_OS_NONE",
        "CONFIG_LV_OS_FREERTOS",
        "CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT",
        "CONFIG_LV_DRAW_THREAD_STACK_SIZE",
        "CONFIG_ESP_LVGL_ADAPTER_FREETYPE_SMALL_RENDER_POOL",
        "CONFIG_APP_MANAGER_LVGL_WORKER_STACK_SIZE",
    }
)
RAM_MATERIALIZED_CONFIG_KEYS = RAM_CONFIG_KEYS | frozenset(
    {
        "CONFIG_LV_DRAW_THREAD_PRIO",
        "CONFIG_LV_USE_FREERTOS_TASK_NOTIFY",
    }
)
NIMBLE_EXTERNAL_CONFIG_KEYS = frozenset(
    {
        "CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL",
        "CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL",
        "CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_DEFAULT",
    }
)
NIMBLE_MATERIALIZED_CONFIG_KEYS = NIMBLE_EXTERNAL_CONFIG_KEYS | frozenset(
    {
        "CONFIG_NIMBLE_MEM_ALLOC_MODE_INTERNAL",
        "CONFIG_NIMBLE_MEM_ALLOC_MODE_EXTERNAL",
    }
)
NIMBLE_INTERNAL_FRAGMENT_EXPECTED = {
    "CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL": "y",
    "CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL": "n",
    "CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_DEFAULT": "n",
}
NIMBLE_ALLOCATOR_CHANGED_CONFIG_KEYS = frozenset(
    {
        "CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL",
        "CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL",
    }
)
STRESS_CONFIG_KEYS = frozenset({"CONFIG_PROVISIONING_SERVICE_DIAGNOSTICS"})
AFFINITY_FRAGMENT_EXPECTED = {
    "CONFIG_FREERTOS_UNICORE": "n",
    "CONFIG_FREERTOS_SMP": "n",
    "CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU0": "y",
    "CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU1": "n",
    "CONFIG_MAIN_PROJECT_TASK_AFFINITY_NO_AFFINITY": "n",
    "CONFIG_APP_MANAGER_LVGL_WORKER_AFFINITY_CPU0": "n",
    "CONFIG_APP_MANAGER_LVGL_WORKER_AFFINITY_CPU1": "y",
    "CONFIG_APP_MANAGER_LVGL_WORKER_AFFINITY_NO_AFFINITY": "n",
    "CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0": "y",
    "CONFIG_ESP_MAIN_TASK_AFFINITY_CPU1": "n",
    "CONFIG_ESP_MAIN_TASK_AFFINITY_NO_AFFINITY": "n",
    "CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0": "y",
    "CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_1": "n",
    "CONFIG_BT_CTRL_PINNED_TO_CORE_0": "y",
    "CONFIG_BT_CTRL_PINNED_TO_CORE_1": "n",
    "CONFIG_BT_NIMBLE_PINNED_TO_CORE_0": "y",
    "CONFIG_BT_NIMBLE_PINNED_TO_CORE_1": "n",
    "CONFIG_LWIP_TCPIP_TASK_AFFINITY_NO_AFFINITY": "n",
    "CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0": "y",
    "CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU1": "n",
    "CONFIG_ESP_TIMER_TASK_AFFINITY_CPU0": "y",
    "CONFIG_ESP_TIMER_TASK_AFFINITY_CPU1": "n",
    "CONFIG_ESP_TIMER_TASK_AFFINITY_NO_AFFINITY": "n",
    "CONFIG_ESP_TIMER_ISR_AFFINITY_CPU0": "y",
    "CONFIG_ESP_TIMER_ISR_AFFINITY_CPU1": "n",
    "CONFIG_ESP_TIMER_ISR_AFFINITY_NO_AFFINITY": "n",
    "CONFIG_FREERTOS_TIMER_TASK_AFFINITY_CPU0": "y",
    "CONFIG_FREERTOS_TIMER_TASK_AFFINITY_CPU1": "n",
    "CONFIG_FREERTOS_TIMER_TASK_NO_AFFINITY": "n",
    "CONFIG_PTHREAD_DEFAULT_CORE_NO_AFFINITY": "n",
    "CONFIG_PTHREAD_DEFAULT_CORE_0": "y",
    "CONFIG_PTHREAD_DEFAULT_CORE_1": "n",
}
AFFINITY_CONFIG_KEYS = frozenset(AFFINITY_FRAGMENT_EXPECTED)
LEGACY_AFFINITY_FRAGMENT_EXPECTED = {
    "CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU0": "n",
    "CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU1": "n",
    "CONFIG_MAIN_PROJECT_TASK_AFFINITY_NO_AFFINITY": "y",
    "CONFIG_APP_MANAGER_LVGL_WORKER_AFFINITY_CPU0": "n",
    "CONFIG_APP_MANAGER_LVGL_WORKER_AFFINITY_CPU1": "n",
    "CONFIG_APP_MANAGER_LVGL_WORKER_AFFINITY_NO_AFFINITY": "y",
    "CONFIG_LWIP_TCPIP_TASK_AFFINITY_NO_AFFINITY": "y",
    "CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0": "n",
    "CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU1": "n",
    "CONFIG_FREERTOS_TIMER_TASK_AFFINITY_CPU0": "n",
    "CONFIG_FREERTOS_TIMER_TASK_AFFINITY_CPU1": "n",
    "CONFIG_FREERTOS_TIMER_TASK_NO_AFFINITY": "y",
    "CONFIG_PTHREAD_DEFAULT_CORE_NO_AFFINITY": "y",
    "CONFIG_PTHREAD_DEFAULT_CORE_0": "n",
    "CONFIG_PTHREAD_DEFAULT_CORE_1": "n",
}
LEGACY_AFFINITY_CONFIG_KEYS = frozenset(
    LEGACY_AFFINITY_FRAGMENT_EXPECTED
)
LEGACY_AFFINITY_CHANGED_CONFIG_KEYS = frozenset(
    {
        "CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU0",
        "CONFIG_MAIN_PROJECT_TASK_AFFINITY_NO_AFFINITY",
        "CONFIG_APP_MANAGER_LVGL_WORKER_AFFINITY_CPU1",
        "CONFIG_APP_MANAGER_LVGL_WORKER_AFFINITY_NO_AFFINITY",
        "CONFIG_LWIP_TCPIP_TASK_AFFINITY_NO_AFFINITY",
        "CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0",
        "CONFIG_FREERTOS_TIMER_TASK_AFFINITY_CPU0",
        "CONFIG_FREERTOS_TIMER_TASK_NO_AFFINITY",
        "CONFIG_PTHREAD_DEFAULT_CORE_NO_AFFINITY",
        "CONFIG_PTHREAD_DEFAULT_CORE_0",
    }
)
AFFINITY_MATERIALIZED_OMITTED_KEYS = frozenset(
    {
        "CONFIG_ESP_TIMER_TASK_AFFINITY_CPU1",
        "CONFIG_ESP_TIMER_TASK_AFFINITY_NO_AFFINITY",
        "CONFIG_ESP_TIMER_ISR_AFFINITY_CPU1",
        "CONFIG_ESP_TIMER_ISR_AFFINITY_NO_AFFINITY",
    }
)
AFFINITY_MATERIALIZED_CONFIG_KEYS = AFFINITY_CONFIG_KEYS | frozenset(
    {
        "CONFIG_MAIN_PROJECT_TASK_CORE_ID",
        "CONFIG_APP_MANAGER_LVGL_WORKER_CORE_ID",
        "CONFIG_ESP_MAIN_TASK_AFFINITY",
        "CONFIG_BT_CTRL_PINNED_TO_CORE",
        "CONFIG_BT_NIMBLE_PINNED_TO_CORE",
        "CONFIG_LWIP_TCPIP_TASK_AFFINITY",
        "CONFIG_ESP_TIMER_TASK_AFFINITY",
        "CONFIG_FREERTOS_TIMER_SERVICE_TASK_CORE_AFFINITY",
        "CONFIG_PTHREAD_TASK_CORE_DEFAULT",
        "CONFIG_FREERTOS_NUMBER_OF_CORES",
        "CONFIG_NIMBLE_PINNED_TO_CORE",
        "CONFIG_NIMBLE_PINNED_TO_CORE_0",
        "CONFIG_NIMBLE_PINNED_TO_CORE_1",
        "CONFIG_ESP32_WIFI_TASK_PINNED_TO_CORE_0",
        "CONFIG_ESP32_WIFI_TASK_PINNED_TO_CORE_1",
        "CONFIG_TCPIP_TASK_AFFINITY",
        "CONFIG_TCPIP_TASK_AFFINITY_NO_AFFINITY",
        "CONFIG_TCPIP_TASK_AFFINITY_CPU0",
        "CONFIG_TCPIP_TASK_AFFINITY_CPU1",
        "CONFIG_ESP32_DEFAULT_PTHREAD_CORE_NO_AFFINITY",
        "CONFIG_ESP32_DEFAULT_PTHREAD_CORE_0",
        "CONFIG_ESP32_DEFAULT_PTHREAD_CORE_1",
    }
)


def project_root() -> Path:
    """Return the MicroTech repository root."""
    return display_profiles.project_root()


def profile_defaults(root: Path, profile: RamProfile) -> tuple[Path, ...]:
    """Return ordered defaults for one fixed board display candidate."""
    assets = display_profiles.config_assets_dir(root)
    defaults = (
        root / "sdkconfig.defaults",
        assets / "benchmark_baseline.defaults",
        assets / profile.defaults,
    )
    defaults += (
        assets / (
            "nimble_external.defaults" if profile.nimble_external else
            "nimble_internal.defaults"
        ),
    )
    if profile.provisioning_diagnostics:
        defaults += (assets / "c_ext_stress.defaults",)
    defaults += (
        assets / (
            "c_ext_affinity.defaults" if profile.task_affinity else
            "c_legacy_affinity.defaults"
        ),
    )
    return defaults


def profile_paths(output_dir: Path,
                  profile: RamProfile) -> display_profiles.ProfilePaths:
    """Return isolated generated paths for one RAM profile."""
    root = output_dir.resolve() / profile.name.lower()
    return display_profiles.ProfilePaths(
        root,
        root / "sdkconfig",
        root / "build",
        root / "source_manifest.json",
        root / "display_benchmark_profile.h",
    )


def benchmark_profile(root: Path, profile: RamProfile) -> dict[str, object]:
    """Return the typed campaign selected by one RAM profile."""
    return display_profiles.load_benchmark_profile(
        display_profiles.benchmark_profiles_dir(root) /
        profile.benchmark_profile
    )


def expected_values(profile: RamProfile) -> dict[str, str]:
    """Return mandatory values in a materialized candidate sdkconfig."""
    values = dict(display_profiles.COMMON_EXPECTED)
    values.update(
        {
            "CONFIG_APP_MANAGER_LVGL_PARTIAL_BUFFER_HEIGHT": "60",
            "CONFIG_APP_MANAGER_PRESENTATION_SNAPSHOT_ANIMATION": "y",
            "CONFIG_LV_OS_NONE": "y" if profile.os_none else "n",
            "CONFIG_LV_OS_FREERTOS": "n" if profile.os_none else "y",
            "CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT": str(profile.draw_units),
            "CONFIG_ESP_LVGL_ADAPTER_FREETYPE_SMALL_RENDER_POOL":
                "y" if profile.small_freetype_pool else "n",
            "CONFIG_APP_MANAGER_LVGL_WORKER_STACK_SIZE":
                "32768" if profile.os_none else "8192",
            "CONFIG_PROVISIONING_SERVICE_DIAGNOSTICS":
                "y" if profile.provisioning_diagnostics else "n",
        }
    )
    if profile.draw_stack_size is not None:
        values["CONFIG_LV_DRAW_THREAD_STACK_SIZE"] = str(
            profile.draw_stack_size
        )
    else:
        values.pop("CONFIG_LV_DRAW_THREAD_STACK_SIZE", None)
    if profile.nimble_external:
        values.update(
            {
                "CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL": "n",
                "CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL": "y",
                "CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_DEFAULT": "n",
            }
        )
    else:
        values.update(NIMBLE_INTERNAL_FRAGMENT_EXPECTED)
    if profile.task_affinity:
        values.update(AFFINITY_FRAGMENT_EXPECTED)
    else:
        values.update(LEGACY_AFFINITY_FRAGMENT_EXPECTED)
    return values


def materialized_expected_values(profile: RamProfile) -> dict[str, str]:
    """Return mandatory values after ESP-IDF expands derived symbols."""
    values = expected_values(profile)
    if profile.nimble_external:
        values.update(
            {
                "CONFIG_NIMBLE_MEM_ALLOC_MODE_INTERNAL": "n",
                "CONFIG_NIMBLE_MEM_ALLOC_MODE_EXTERNAL": "y",
            }
        )
    else:
        values.update(
            {
                "CONFIG_NIMBLE_MEM_ALLOC_MODE_INTERNAL": "y",
                "CONFIG_NIMBLE_MEM_ALLOC_MODE_EXTERNAL": "n",
            }
        )
    if profile.task_affinity:
        for key in AFFINITY_MATERIALIZED_OMITTED_KEYS:
            values.pop(key)
        values.update(
            {
                "CONFIG_MAIN_PROJECT_TASK_CORE_ID": "0x0",
                "CONFIG_APP_MANAGER_LVGL_WORKER_CORE_ID": "1",
                "CONFIG_ESP_MAIN_TASK_AFFINITY": "0x0",
                "CONFIG_BT_CTRL_PINNED_TO_CORE": "0",
                "CONFIG_BT_NIMBLE_PINNED_TO_CORE": "0",
                "CONFIG_LWIP_TCPIP_TASK_AFFINITY": "0x0",
                "CONFIG_ESP_TIMER_TASK_AFFINITY": "0x0",
                "CONFIG_FREERTOS_TIMER_SERVICE_TASK_CORE_AFFINITY": "0x0",
                "CONFIG_PTHREAD_TASK_CORE_DEFAULT": "0",
                "CONFIG_FREERTOS_NUMBER_OF_CORES": "2",
            }
        )
    else:
        values.update(
            {
                "CONFIG_MAIN_PROJECT_TASK_CORE_ID": "0x7FFFFFFF",
                "CONFIG_APP_MANAGER_LVGL_WORKER_CORE_ID": "-1",
                "CONFIG_LWIP_TCPIP_TASK_AFFINITY": "0x7FFFFFFF",
                "CONFIG_FREERTOS_TIMER_SERVICE_TASK_CORE_AFFINITY":
                    "0x7FFFFFFF",
                "CONFIG_PTHREAD_TASK_CORE_DEFAULT": "-1",
                "CONFIG_FREERTOS_NUMBER_OF_CORES": "2",
            }
        )
    return values


def _assert_expected(name: str, values: dict[str, str],
                     profile: RamProfile, materialized: bool) -> None:
    expected = materialized_expected_values(profile) if materialized else \
        expected_values(profile)
    if not materialized:
        expected.pop("CONFIG_APP_MANAGER_LVGL_WORKER_STACK_SIZE")
    mismatches = []
    for key, expected_value in expected.items():
        actual = values.get(key, "<missing>")
        if actual != expected_value:
            mismatches.append(f"{key}={actual} (expected {expected_value})")
    if profile.draw_stack_size is None:
        actual_stack = values.get("CONFIG_LV_DRAW_THREAD_STACK_SIZE")
        if actual_stack not in (None, "n"):
            mismatches.append(
                "CONFIG_LV_DRAW_THREAD_STACK_SIZE="
                f"{actual_stack} (expected inactive)"
            )
    if mismatches:
        raise ProfileError(f"{name} has invalid settings: " +
                           ", ".join(mismatches))


def validate_assets(root: Path) -> None:
    """Validate candidate fragments and fixed display settings."""
    display_profiles.validate_assets(root)
    assets = display_profiles.config_assets_dir(root)
    for name in PROFILE_ORDER:
        profile = PROFILES[name]
        fragment = display_profiles.parse_config(assets / profile.defaults)
        expected_fragment = {
            "CONFIG_LV_OS_NONE": "y" if profile.os_none else "n",
            "CONFIG_LV_OS_FREERTOS": "n" if profile.os_none else "y",
            "CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT": str(profile.draw_units),
            "CONFIG_LV_DRAW_THREAD_STACK_SIZE":
                str(profile.draw_stack_size)
                if profile.draw_stack_size is not None else "n",
            "CONFIG_ESP_LVGL_ADAPTER_FREETYPE_SMALL_RENDER_POOL":
                "y" if profile.small_freetype_pool else "n",
        }
        if fragment != expected_fragment:
            raise ProfileError(
                f"{profile.defaults} has unexpected settings: {fragment}"
            )
        merged = display_profiles.merge_configs(
            profile_defaults(root, profile)
        )
        _assert_expected(f"{name} defaults", merged, profile, False)

    baseline = display_profiles.merge_configs(
        profile_defaults(root, PROFILES["B0"])
    )
    for name in PROFILE_ORDER[1:]:
        candidate = display_profiles.merge_configs(
            profile_defaults(root, PROFILES[name])
        )
        unexpected = display_profiles.differing_keys(
            baseline, candidate
        ) - RAM_CONFIG_KEYS
        if unexpected:
            raise ProfileError(
                f"B0/{name} defaults differ outside LVGL RAM settings: "
                f"{sorted(unexpected)}"
            )

    nimble_fragment = display_profiles.parse_config(
        assets / "nimble_external.defaults"
    )
    expected_nimble_fragment = {
        "CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL": "n",
        "CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL": "y",
        "CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_DEFAULT": "n",
    }
    if nimble_fragment != expected_nimble_fragment:
        raise ProfileError(
            "nimble_external.defaults has unexpected settings: "
            f"{nimble_fragment}"
        )
    internal_nimble_fragment = display_profiles.parse_config(
        assets / "nimble_internal.defaults"
    )
    if internal_nimble_fragment != NIMBLE_INTERNAL_FRAGMENT_EXPECTED:
        raise ProfileError(
            "nimble_internal.defaults has unexpected settings: "
            f"{internal_nimble_fragment}"
        )
    synchronous = display_profiles.merge_configs(
        profile_defaults(root, PROFILES["C"])
    )
    external = display_profiles.merge_configs(
        profile_defaults(root, DIAGNOSTIC_PROFILES["C_EXT"])
    )
    unexpected = display_profiles.differing_keys(
        synchronous, external
    ) - (NIMBLE_EXTERNAL_CONFIG_KEYS | LEGACY_AFFINITY_CONFIG_KEYS)
    if unexpected:
        raise ProfileError(
            "C/C_EXT defaults differ outside NimBLE and affinity settings: "
            f"{sorted(unexpected)}"
        )
    stress_fragment = display_profiles.parse_config(
        assets / "c_ext_stress.defaults"
    )
    if stress_fragment != {
            "CONFIG_PROVISIONING_SERVICE_DIAGNOSTICS": "y"}:
        raise ProfileError(
            "c_ext_stress.defaults has unexpected settings: "
            f"{stress_fragment}"
        )
    affinity_fragment = display_profiles.parse_config(
        assets / "c_ext_affinity.defaults"
    )
    if affinity_fragment != AFFINITY_FRAGMENT_EXPECTED:
        raise ProfileError(
            "c_ext_affinity.defaults has unexpected settings: "
            f"{affinity_fragment}"
        )
    legacy_affinity_fragment = display_profiles.parse_config(
        assets / "c_legacy_affinity.defaults"
    )
    if legacy_affinity_fragment != LEGACY_AFFINITY_FRAGMENT_EXPECTED:
        raise ProfileError(
            "c_legacy_affinity.defaults has unexpected settings: "
            f"{legacy_affinity_fragment}"
        )
    if AFFINITY_FRAGMENT_EXPECTED[
            "CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU0"] != "y" or \
            AFFINITY_FRAGMENT_EXPECTED[
                "CONFIG_APP_MANAGER_LVGL_WORKER_AFFINITY_CPU1"] != "y":
        raise ProfileError("C_EXT task cores are not isolated")
    stress = display_profiles.merge_configs(
        profile_defaults(root, DIAGNOSTIC_PROFILES["C_EXT_STRESS"])
    )
    unexpected = display_profiles.differing_keys(
        external, stress
    ) - STRESS_CONFIG_KEYS
    if unexpected:
        raise ProfileError(
            "C_EXT/C_EXT_STRESS defaults differ outside provisioning "
            f"diagnostics: {sorted(unexpected)}"
        )


def build_command(root: Path, output_dir: Path,
                  profile: RamProfile) -> list[str]:
    """Return the isolated IDF build and size command for one candidate."""
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


def validate_materialized_profile(path: Path,
                                  profile: RamProfile) -> dict[str, str]:
    """Validate one sdkconfig generated by ESP-IDF."""
    values = display_profiles.parse_config(path)
    _assert_expected(profile.name, values, profile, True)
    return values


def prepare(root: Path, output_dir: Path, reset: bool,
            profile_names: Iterable[str] = PRIMARY_PROFILE_ORDER) -> list[str]:
    """Prepare isolated candidate directories and return build commands."""
    validate_assets(root)
    display_profiles._ensure_isolated(root, output_dir)
    state = display_profiles.source_manifest(root)
    commands = []
    for name in profile_names:
        profile = ALL_PROFILES[name]
        expected_header = display_profiles.render_profile_header(
            benchmark_profile(root, profile)
        )
        paths = profile_paths(output_dir, profile)
        if reset:
            if paths.build.is_symlink():
                paths.build.unlink()
            elif paths.build.exists():
                shutil.rmtree(paths.build)
            for path in (
                    paths.sdkconfig, paths.sdkconfig.with_suffix(".old"),
                    paths.manifest, paths.header):
                if path.exists() or path.is_symlink():
                    path.unlink()
        paths.root.mkdir(parents=True, exist_ok=True)
        expected_manifest = dict(state)
        expected_manifest["profile"] = profile.name
        if paths.manifest.exists():
            if display_profiles._read_profile_manifest(
                    paths.manifest) != expected_manifest:
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
            display_profiles._write_profile_manifest(
                paths.manifest, profile, state
            )
        if paths.header.exists():
            if paths.header.read_text(encoding="utf-8") != expected_header:
                raise ProfileError(
                    f"{profile.name} generated profile changed; rerun "
                    "prepare with --reset"
                )
        else:
            paths.header.write_text(expected_header, encoding="utf-8")
        if paths.sdkconfig.exists():
            validate_materialized_profile(paths.sdkconfig, profile)
        commands.append(shlex.join(build_command(root, output_dir, profile)))
    return commands


def validate_profile_artifacts(root: Path, output_dir: Path,
                               profile: RamProfile,
                               state: dict[str, object]) -> dict[str, str]:
    """Validate one candidate config, image, profile, and source stamp."""
    paths = profile_paths(output_dir, profile)
    values = validate_materialized_profile(paths.sdkconfig, profile)
    expected_header = display_profiles.render_profile_header(
        benchmark_profile(root, profile)
    )
    if not paths.header.is_file() or paths.header.read_text(
            encoding="utf-8") != expected_header:
        raise ProfileError(
            f"{profile.name} generated profile header is invalid"
        )
    if not (paths.build / "microtech.bin").is_file():
        raise ProfileError(f"{profile.name} firmware image is missing")
    expected_manifest = dict(state)
    expected_manifest["profile"] = profile.name
    if display_profiles._read_profile_manifest(
            paths.manifest) != expected_manifest:
        raise ProfileError(
            f"{profile.name} source manifest does not match current sources"
        )
    return values


def validate_matrix(root: Path, output_dir: Path,
                    profile_names: Iterable[str]) -> None:
    """Validate materialized profiles and reject unrelated config drift."""
    names = tuple(profile_names)
    state = display_profiles.source_manifest(root)
    values = {
        name: validate_profile_artifacts(
            root, output_dir, ALL_PROFILES[name], state
        ) for name in names
    }
    if "C_EXT" in values and "C_EXT_STRESS" in values:
        unexpected = display_profiles.differing_keys(
            values["C_EXT"], values["C_EXT_STRESS"]
        ) - STRESS_CONFIG_KEYS
        if unexpected:
            raise ProfileError(
                "C_EXT/C_EXT_STRESS materialized configs differ outside "
                f"provisioning diagnostics: {sorted(unexpected)}"
            )
    if "B0" not in values:
        return
    for name in names:
        allowed = RAM_MATERIALIZED_CONFIG_KEYS
        if ALL_PROFILES[name].nimble_external:
            allowed |= NIMBLE_MATERIALIZED_CONFIG_KEYS
        if ALL_PROFILES[name].provisioning_diagnostics:
            allowed |= STRESS_CONFIG_KEYS
        if ALL_PROFILES[name].task_affinity:
            allowed |= AFFINITY_MATERIALIZED_CONFIG_KEYS
        unexpected = display_profiles.differing_keys(
            values["B0"], values[name]
        ) - allowed
        if unexpected:
            raise ProfileError(
                f"B0/{name} materialized configs differ outside LVGL RAM "
                f"settings: {sorted(unexpected)}"
            )


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Prepare and validate LVGL internal-RAM profiles"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    prepare_parser = subparsers.add_parser("prepare")
    prepare_parser.add_argument(
        "--output-dir", type=Path, default=Path("/tmp/mt-lvgl-ram")
    )
    prepare_parser.add_argument("--reset", action="store_true")
    prepare_parser.add_argument(
        "--profiles", nargs="+", choices=ALL_PROFILE_ORDER,
        default=PRIMARY_PROFILE_ORDER,
    )
    command_parser = subparsers.add_parser("command")
    command_parser.add_argument("profile", choices=ALL_PROFILE_ORDER)
    command_parser.add_argument(
        "--output-dir", type=Path, default=Path("/tmp/mt-lvgl-ram")
    )
    validate_parser = subparsers.add_parser("validate")
    validate_parser.add_argument(
        "--output-dir", type=Path, default=Path("/tmp/mt-lvgl-ram")
    )
    validate_parser.add_argument(
        "--profiles", nargs="+", choices=ALL_PROFILE_ORDER,
        default=PRIMARY_PROFILE_ORDER,
    )
    return parser.parse_args()


def main() -> int:
    """Run the requested profile operation."""
    args = _parse_args()
    root = project_root()
    try:
        if args.command == "prepare":
            for command in prepare(
                    root, args.output_dir, args.reset, args.profiles):
                print(command)
        elif args.command == "command":
            validate_assets(root)
            display_profiles._ensure_isolated(root, args.output_dir)
            print(shlex.join(build_command(
                root, args.output_dir, ALL_PROFILES[args.profile]
            )))
        else:
            validate_assets(root)
            display_profiles._ensure_isolated(root, args.output_dir)
            validate_matrix(root, args.output_dir, args.profiles)
            for name in args.profiles:
                print(f"{name}: PASS")
    except ProfileError as error:
        print(f"error: {error}")
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
