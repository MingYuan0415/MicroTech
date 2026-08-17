#!/usr/bin/env python3
"""Read-only checks for production configuration ownership rules."""

from __future__ import annotations

import json
import re
import subprocess
import unittest
from pathlib import Path

import yaml


CONFIG_FALLBACK = re.compile(r"^\s*#\s*ifndef\s+CONFIG_[A-Z0-9_]+", re.MULTILINE)
TICK_REDEFINITION = re.compile(
    r"^\s*#\s*define\s+configTICK_RATE_HZ\b", re.MULTILINE
)
UNPINNED_TASK_CREATE = (
    re.compile(r"\bxTaskCreate\s*\("),
    re.compile(r"\bxTaskCreateStatic\s*\("),
    re.compile(r"\bxTaskCreateWithCaps\s*\("),
)
DEPRECATED_CONFIG_TOKENS = (
    "CONFIG_BSP_AUDIO_",
    "CONFIG_AUDIO_SERVICE_",
    "CONFIG_SD_STORAGE_SERVICE_",
    "CONFIG_IMU_SERVICE_SAMPLE_RATE_HZ",
    "CONFIG_IMU_SERVICE_TASK_PRIORITY",
    "CONFIG_POWER_SERVICE_TASK_PRIORITY",
    "CONFIG_POWER_SERVICE_POLL_INTERVAL_MS",
    "CONFIG_POWER_SERVICE_IRQ_POLL_INTERVAL_MS",
    "CONFIG_WIFI_SERVICE_TASK_PRIORITY",
    "CONFIG_WIFI_SERVICE_WORKER_POLL_MS",
    "CONFIG_WIFI_SERVICE_EVENT_DRAIN_TIMEOUT_MS",
    "CONFIG_SYSTEM_PM_STANDBY_TASK_PRIO",
    "CONFIG_APP_MANAGER_DISPLAY_BENCHMARK",
    "CONFIG_APP_MANAGER_LVGL_RGB565_SWAPPED",
    "CONFIG_BSP_DISPLAY_NON_TE_PSRAM_DMA_DIRECT",
    "CONFIG_BSP_DISPLAY_SPI_CLOCK_40M",
    "CONFIG_BSP_DISPLAY_SPI_CLOCK_80M",
    "CONFIG_BSP_DISPLAY_SPI_CLOCK_HZ",
    "CONFIG_BSP_DISPLAY_SPI_MAX_TRANSFER_LINES",
    "CONFIG_BSP_DISPLAY_SPI_TRANS_QUEUE_DEPTH",
    "CONFIG_BSP_DISPLAY_TE_SYNC",
    "CONFIG_SYSTEM_PM_DEVELOPMENT_MODE",
)


class ConfigurationGovernanceTest(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(__file__).resolve().parents[2]

    def _production_files(self):
        roots = (self.root / "main", self.root / "layers")
        for source_root in roots:
            for path in source_root.rglob("*"):
                if not path.is_file():
                    continue
                relative = path.relative_to(self.root)
                if any(part in {"tests", "managed_components", "XPowersLib",
                                "build", "probe"}
                       for part in relative.parts):
                    continue
                if path.name in {"Kconfig", "Kconfig.projbuild", "CMakeLists.txt"} or \
                        path.suffix in {".c", ".h", ".cmake"}:
                    yield relative, path

    def test_production_configuration_rules(self) -> None:
        failures = []
        for relative, path in self._production_files():
            text = path.read_text(encoding="utf-8")
            if CONFIG_FALLBACK.search(text):
                failures.append(f"{relative}: CONFIG fallback")
            if TICK_REDEFINITION.search(text):
                failures.append(f"{relative}: configTICK_RATE_HZ redefinition")
            if path.suffix in {".c", ".h"} and any(
                    pattern.search(text) for pattern in UNPINNED_TASK_CREATE):
                failures.append(f"{relative}: unpinned task creation")
            for token in DEPRECATED_CONFIG_TOKENS:
                if token in text:
                    failures.append(f"{relative}: deprecated {token}")
        self.assertEqual(failures, [], "\n".join(failures))

    def test_removed_configuration_is_absent_from_defaults(self) -> None:
        paths = [self.root / "sdkconfig.defaults"]
        paths.extend(sorted(
            (self.root / "tests/display/profile_defaults").glob("*.defaults")
        ))
        failures = []
        for path in paths:
            text = path.read_text(encoding="utf-8")
            for token in DEPRECATED_CONFIG_TOKENS[-9:]:
                if token in text:
                    failures.append(
                        f"{path.relative_to(self.root)}: deprecated {token}"
                    )
        self.assertEqual(failures, [], "\n".join(failures))

    def test_project_kconfig_symbol_count(self) -> None:
        symbols = []
        for path in self.root.rglob("Kconfig*"):
            relative = path.relative_to(self.root)
            if any(part in {"managed_components", "build"}
                   for part in relative.parts):
                continue
            symbols.extend(re.findall(
                r"^\s*config\s+([A-Z0-9_]+)\b",
                path.read_text(encoding="utf-8"),
                re.MULTILINE,
            ))
        self.assertEqual(len(symbols), 53, sorted(symbols))

    def test_connectivity_defaults(self) -> None:
        defaults = (self.root / "sdkconfig.defaults").read_text(encoding="utf-8")
        assignments = dict(re.findall(
            r"^(CONFIG_[A-Z0-9_]+)=(.+)$", defaults, re.MULTILINE
        ))
        expected = {
            "CONFIG_BT_ENABLED": "y",
            "CONFIG_BT_NIMBLE_ENABLED": "y",
            "CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL": "y",
            "CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE": "6144",
            "CONFIG_BT_NIMBLE_ROLE_CENTRAL": "n",
            "CONFIG_BT_NIMBLE_ROLE_PERIPHERAL": "y",
            "CONFIG_BT_NIMBLE_ROLE_BROADCASTER": "y",
            "CONFIG_BT_NIMBLE_ROLE_OBSERVER": "n",
            "CONFIG_BT_NIMBLE_GATT_CLIENT": "n",
            "CONFIG_BT_NIMBLE_GATT_SERVER": "y",
            "CONFIG_BT_NIMBLE_SECURITY_ENABLE": "y",
            "CONFIG_BT_NIMBLE_SM_LEGACY": "n",
            "CONFIG_BT_NIMBLE_SM_SC": "y",
            "CONFIG_BT_NIMBLE_SM_SC_ONLY": "1",
            "CONFIG_BT_NIMBLE_NVS_PERSIST": "y",
            "CONFIG_BT_NIMBLE_MAX_BONDS": "1",
            "CONFIG_BT_NIMBLE_MAX_CONNECTIONS": "1",
            "CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU": "498",
            "CONFIG_BT_CTRL_BLE_MAX_ACT": "2",
            "CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU0": "y",
            "CONFIG_APP_MANAGER_LVGL_WORKER_AFFINITY_CPU1": "y",
            "CONFIG_APP_MANAGER_LVGL_WORKER_STACK_SIZE": "32768",
            "CONFIG_APP_MANAGER_NAV_COMMAND_CAPACITY": "8",
            "CONFIG_APP_MANAGER_MAILBOX_CAPACITY": "12",
            "CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0": "y",
            "CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0": "y",
            "CONFIG_BT_CTRL_PINNED_TO_CORE_0": "y",
            "CONFIG_BT_NIMBLE_PINNED_TO_CORE_0": "y",
            "CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0": "y",
            "CONFIG_ESP_TIMER_TASK_AFFINITY_CPU0": "y",
            "CONFIG_ESP_TIMER_ISR_AFFINITY_CPU0": "y",
            "CONFIG_FREERTOS_TIMER_TASK_AFFINITY_CPU0": "y",
            "CONFIG_PTHREAD_DEFAULT_CORE_0": "y",
            "CONFIG_ESP_WIFI_NVS_ENABLED": "y",
            "CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC": "y",
            "CONFIG_MBEDTLS_DYNAMIC_BUFFER": "y",
            "CONFIG_LV_USE_QRCODE": "y",
            "CONFIG_LV_OS_NONE": "y",
            "CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT": "1",
            "CONFIG_CONNECTIVITY_MANAGER_TASK_STACK": "6144",
            "CONFIG_DEVICE_LINK_SERVICE_TASK_STACK": "6144",
            "CONFIG_MAIN_WEATHER_SERVER_BASE_URL":
                '"https://weather.example.com"',
            "CONFIG_MAIN_WEATHER_DEVICE_TOKEN": '"example-device-token"',
        }
        self.assertEqual(
            {key: assignments.get(key) for key in expected}, expected
        )
        self.assertRegex(
            (
                self.root /
                "layers/middleware/components/connectivity_manager/Kconfig"
            ).read_text(encoding="utf-8"),
            r"config CONNECTIVITY_MANAGER_TASK_STACK\s+"
            r"int[^\n]*\s+default 6144\b\s+range 4096 8192\b",
        )

    def test_device_link_wifi_gate_matches_contract_registry(self) -> None:
        registry = yaml.safe_load((
            self.root / "contracts/provisioning/registry/domains.yaml"
        ).read_text(encoding="utf-8"))
        wifi = next(domain for domain in registry["domains"]
                    if domain["name"] == "wifi")
        registry_advertised = wifi["advertised"]

        kconfig_text = (
            self.root /
            "layers/middleware/components/device_link_service/Kconfig"
        ).read_text(encoding="utf-8")
        gate = re.search(
            r"(?ms)^config DEVICE_LINK_SERVICE_WIFI_ADVERTISED\b.*?(?=^config |^endmenu)",
            kconfig_text,
        )
        self.assertIsNotNone(gate)
        default = re.search(r"(?m)^\s*default\s+(y|n)\s*$",
                            gate.group(0))
        self.assertIsNotNone(default)
        kconfig_advertised = default.group(1) == "y"

        production_defaults = (self.root / "sdkconfig.defaults").read_text(
            encoding="utf-8"
        )
        symbol = "CONFIG_DEVICE_LINK_SERVICE_WIFI_ADVERTISED"
        production_advertised = bool(re.search(
            rf"(?m)^{symbol}=y$", production_defaults
        ))
        self.assertRegex(production_defaults,
                         rf"(?m)^# {symbol} is not set$")
        self.assertEqual(
            [registry_advertised, kconfig_advertised,
             production_advertised],
            [False, False, False],
        )

        exceptions = json.loads((
            self.root / "tests/configuration/device_link_gate_overlays.json"
        ).read_text(encoding="utf-8"))
        self.assertEqual(set(exceptions), {"schema_version", "exceptions"})
        self.assertEqual(exceptions["schema_version"], 1)
        self.assertEqual(len(exceptions["exceptions"]), 1)
        exception = exceptions["exceptions"][0]
        self.assertEqual(
            set(exception),
            {"overlay", "domain", "kconfig", "advertised", "purpose"},
        )
        self.assertEqual(exception["domain"], "wifi")
        self.assertEqual(exception["kconfig"], symbol)
        self.assertTrue(exception["advertised"])
        overlay = (self.root / exception["overlay"]).read_text(
            encoding="utf-8"
        )
        self.assertRegex(overlay, rf"(?m)^{symbol}=y$")

    def test_device_link_security_profile_consistency(self) -> None:
        profile = yaml.safe_load((
            self.root /
            "contracts/provisioning/profiles/device-link/v2/profile.yaml"
        ).read_text(encoding="utf-8"))
        methods = json.loads((
            self.root / "contracts/provisioning/fixtures/core/v2/methods.json"
        ).read_text(encoding="utf-8"))
        manifest = next(case for case in methods
                        if case["id"] == "get-manifest")
        fixture = manifest["response"]["value"]
        profile_security = profile["security"]
        security_mapping = {
            "secure_connections_only": "le_secure_connections_only",
            "encryption_key_bytes": "encryption_key_bytes",
            "maximum_bonds": "maximum_bonds",
            "protocomm_security_version": "protocomm_security_version",
            "protocomm_security_patch_version":
                "protocomm_security_patch_version",
            "local_confirmation_for_grants":
                "local_confirmation_for_grants",
            "qr_bootstrap_uses_pop": "qr_bootstrap_uses_pop",
            "public_bootstrap_uses_sc_local_confirmation":
                "public_bootstrap_uses_sc_local_confirmation",
        }
        for profile_key, manifest_key in security_mapping.items():
            self.assertEqual(profile_security[profile_key],
                             fixture["security"][manifest_key])

        header = (
            self.root /
            "layers/middleware/components/device_link/include/device_link_protocol.h"
        ).read_text(encoding="utf-8")

        def macro_value(name: str):
            match = re.search(rf"(?m)^#define {name}\s+(\S+)$", header)
            self.assertIsNotNone(match, name)
            value = match.group(1)
            if value in {"true", "false"}:
                return value == "true"
            return int(value.removesuffix("U"))

        firmware_mapping = {
            "secure_connections_only":
                "DEVICE_LINK_SECURITY_SECURE_CONNECTIONS_ONLY",
            "encryption_key_bytes":
                "DEVICE_LINK_SECURITY_ENCRYPTION_KEY_BYTES",
            "maximum_bonds": "DEVICE_LINK_SECURITY_MAXIMUM_BONDS",
            "protocomm_security_version":
                "DEVICE_LINK_SECURITY_PROTOCOMM_VERSION",
            "protocomm_security_patch_version":
                "DEVICE_LINK_SECURITY_PROTOCOMM_PATCH_VERSION",
            "local_confirmation_for_grants":
                "DEVICE_LINK_SECURITY_LOCAL_CONFIRMATION_FOR_GRANTS",
            "qr_bootstrap_uses_pop":
                "DEVICE_LINK_SECURITY_QR_BOOTSTRAP_USES_POP",
            "public_bootstrap_uses_sc_local_confirmation":
                "DEVICE_LINK_SECURITY_PUBLIC_BOOTSTRAP_USES_SC_CONFIRMATION",
        }
        for profile_key, macro in firmware_mapping.items():
            self.assertEqual(profile_security[profile_key],
                             macro_value(macro))
        self.assertEqual(
            fixture["protocol_version"],
            {"major": macro_value("DEVICE_LINK_CORE_MAJOR"),
             "minor": macro_value("DEVICE_LINK_CORE_MINOR")},
        )
        self.assertEqual(
            fixture["profile_version"],
            {"major": macro_value("DEVICE_LINK_PROFILE_MAJOR"),
             "minor": macro_value("DEVICE_LINK_PROFILE_MINOR")},
        )

        defaults = (self.root / "sdkconfig.defaults").read_text(
            encoding="utf-8"
        )
        assignments = dict(re.findall(
            r"^(CONFIG_[A-Z0-9_]+)=(.+)$", defaults, re.MULTILINE
        ))
        self.assertEqual(assignments["CONFIG_BT_NIMBLE_SM_SC"], "y")
        self.assertEqual(assignments["CONFIG_BT_NIMBLE_SM_LEGACY"], "n")
        self.assertEqual(assignments["CONFIG_BT_NIMBLE_SM_SC_ONLY"], "1")
        self.assertEqual(int(assignments["CONFIG_BT_NIMBLE_MAX_BONDS"]),
                         profile_security["maximum_bonds"])

    def test_device_link_security_release_target_is_wired(self) -> None:
        cmake = (
            self.root /
            "layers/middleware/components/ble_runtime/tests/host/CMakeLists.txt"
        ).read_text(encoding="utf-8")
        self.assertIn("add_custom_target(device_link_security_release", cmake)
        self.assertIn("DEVICE_LINK_REQUIRE_SECURITY_RELEASE=1", cmake)
        release = (self.root /
                   "tests/device_link_security_release.sh").read_text(
                       encoding="utf-8")
        self.assertIn("--target device_link_security_release", release)

    def test_device_link_contract_lock_name_and_commit_marker(self) -> None:
        component = self.root / "layers/middleware/components/device_link"
        lock = component / "device-link-contract.lock"
        self.assertTrue(lock.is_file())
        self.assertFalse((component / "contract.lock").exists())
        lock_values = dict(
            line.split("=", 1)
            for line in lock.read_text(encoding="utf-8").splitlines()
        )
        cmake = (self.root / "main/CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        expected = re.search(
            r'EXPECTED_CONTRACT_COMMIT="([0-9a-f]{40})"', cmake
        )
        self.assertIsNotNone(expected)
        self.assertEqual(lock_values.get("contract_commit"),
                         expected.group(1))
        contract = self.root / "contracts/provisioning"
        contract_head = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=contract, check=True,
            capture_output=True, text=True,
        ).stdout.strip()
        digest = subprocess.run(
            ["python3", "-m", "tooling.contractcheck.cli",
             "--print-digest"], cwd=contract, check=True,
            capture_output=True, text=True,
        ).stdout.strip()
        self.assertEqual(lock_values.get("contract_commit"), contract_head)
        self.assertEqual(lock_values.get("schema_digest"), digest)
        readme = (component / "README.md").read_text(encoding="utf-8")
        self.assertIn(
            "expectedContractCommit=" + expected.group(1),
            readme.splitlines(),
        )


if __name__ == "__main__":
    unittest.main()
