#!/usr/bin/env python3
"""Read-only checks for production configuration ownership rules."""

from __future__ import annotations

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
    "CONFIG_CONNECTIVITY_MANAGER_MAX_PROFILES",
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
        paths.extend([
            self.root / "sdkconfig.defaults.esp32s3",
            self.root / "sdkconfig.defaults.esp32p4",
        ])
        failures = []
        for path in paths:
            text = path.read_text(encoding="utf-8")
            for token in DEPRECATED_CONFIG_TOKENS[-9:]:
                if token in text:
                    failures.append(
                        f"{path.relative_to(self.root)}: deprecated {token}"
                    )
        self.assertEqual(failures, [], "\n".join(failures))

    def test_connectivity_defaults(self) -> None:
        defaults = "\n".join(
            (self.root / name).read_text(encoding="utf-8")
            for name in ("sdkconfig.defaults", "sdkconfig.defaults.esp32s3")
        )
        assignments = dict(re.findall(
            r"^(CONFIG_[A-Z0-9_]+)=(.+)$", defaults, re.MULTILINE
        ))
        expected = {
            "CONFIG_BT_ENABLED": "y",
            "CONFIG_BT_NIMBLE_ENABLED": "y",
            "CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL": "y",
            "CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE": "6144",
            "CONFIG_BT_NIMBLE_ROLE_CENTRAL": "n",
            "CONFIG_BT_NIMBLE_ROLE_PERIPHERAL": "y",
            "CONFIG_BT_NIMBLE_ROLE_BROADCASTER": "y",
            "CONFIG_BT_NIMBLE_ROLE_OBSERVER": "n",
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
            "CONFIG_CONNECTIVITY_MANAGER_TASK_STACK": "8192",
            "CONFIG_DEVICE_LINK_SERVICE_TASK_STACK": "6144",
            "CONFIG_MAIN_WEATHER_SERVER_BASE_URL":
                '"https://weather.example.com"',
            "CONFIG_MAIN_WEATHER_DEVICE_TOKEN": '"example-device-token"',
        }
        self.assertEqual(
            {key: assignments.get(key) for key in expected}, expected
        )
        # Symbols whose Kconfig `depends on` makes them invisible under the
        # pinned roles above: GATT client needs ROLE_CENTRAL and the LVGL draw
        # unit count needs !LV_OS_NONE. Assert the implied effective value
        # rather than a literal line that save-defconfig can never emit.
        self.assertNotIn("CONFIG_BT_NIMBLE_GATT_CLIENT", assignments)
        self.assertNotIn("CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT", assignments)
        self.assertRegex(
            (
                self.root /
                "layers/middleware/components/connectivity_manager/Kconfig"
            ).read_text(encoding="utf-8"),
            r"config CONNECTIVITY_MANAGER_TASK_STACK\s+"
            r"int[^\n]*\s+default 8192\b\s+range 4096 8192\b",
        )

    def test_target_profile_board_symbols(self) -> None:
        profiles = {
            "sdkconfig.defaults.esp32s3":
                "CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_AMOLED_1_8=y",
            "sdkconfig.defaults.esp32p4":
                "CONFIG_BOARD_TYPE_WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_3_5=y",
        }
        for name, symbol in profiles.items():
            text = (self.root / name).read_text(encoding="utf-8")
            self.assertIn(symbol, text, f"{name} lacks its board symbol")
            self.assertNotIn("CONFIG_IDF_TARGET=", text,
                             f"{name} must not pin IDF_TARGET")

    def test_device_link_contract_source_is_authoritative(self) -> None:
        contract = self.root / "contracts/device_link"
        self.assertTrue((contract / "protocol.yaml").is_file())
        self.assertTrue((contract / "vectors/golden.json").is_file())
        self.assertEqual(
            (contract / "VERSION").read_text(encoding="utf-8").strip(),
            "1.0.0",
        )
        gitmodules = (self.root / ".gitmodules").read_text(encoding="utf-8")
        self.assertIn("path = contracts/device_link", gitmodules)
        self.assertIn(
            "url = https://github.com/MingYuan0415/mt-device-link-contract.git",
            gitmodules,
        )

    def test_device_link_contract_lock_matches_submodule(self) -> None:
        # The middleware component owns the immutable contract pin. The root
        # repository must not keep a second lock that can drift from the
        # submodule pointer.
        self.assertFalse((self.root / "contracts/device_link.lock").exists())
        lock = (self.root / "layers/middleware/components/device_link" /
                "device-link-contract.lock")
        self.assertTrue(lock.is_file())
        lines = lock.read_text(encoding="utf-8").splitlines()
        self.assertEqual([line.split("=", 1)[0] for line in lines], [
            "lock_format", "contract_commit", "profile", "schema_format",
            "schema_digest", "release_state", "release_tag", "pin_state",
        ])
        lock_values = dict(line.split("=", 1) for line in lines)
        contract = self.root / "contracts/device_link"
        contract_head = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=contract, check=True,
            capture_output=True, text=True,
        ).stdout.strip()
        digest = subprocess.run(
            ["python3", "-m", "tooling.check", "--print-digest"],
            cwd=contract, check=True, capture_output=True, text=True,
        ).stdout.strip()
        optimized_digest = subprocess.run(
            ["python3", "-O", "-m", "tooling.check", "--print-digest"],
            cwd=contract, check=True, capture_output=True, text=True,
        ).stdout.strip()
        self.assertEqual(digest, optimized_digest)
        self.assertEqual(lock_values["lock_format"], "2")
        self.assertEqual(lock_values["contract_commit"], contract_head)
        self.assertEqual(lock_values["schema_digest"], digest)
        self.assertEqual(lock_values["profile"], "device-link/v1")
        self.assertEqual(lock_values["schema_format"], "fixed-binary/1")
        self.assertEqual(lock_values["release_state"], "freeze_candidate")
        self.assertEqual(lock_values["release_tag"], "none")
        self.assertEqual(lock_values["pin_state"], "working_tree_candidate")


if __name__ == "__main__":
    unittest.main()
