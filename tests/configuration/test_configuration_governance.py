#!/usr/bin/env python3
"""Read-only checks for production configuration ownership rules."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


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
                if any(part in {"tests", "managed_components", "XPowersLib", "build"}
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
        self.assertEqual(len(symbols), 42, sorted(symbols))

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
            "CONFIG_BT_NIMBLE_SECURITY_ENABLE": "n",
            "CONFIG_BT_NIMBLE_NVS_PERSIST": "n",
            "CONFIG_BT_NIMBLE_MAX_CONNECTIONS": "1",
            "CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU": "500",
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
            "CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_2": "y",
            "CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_PATCH_VERSION": "y",
            "CONFIG_LV_USE_QRCODE": "y",
            "CONFIG_LV_OS_NONE": "y",
            "CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT": "1",
            "CONFIG_CONNECTIVITY_MANAGER_TASK_STACK": "6144",
            "CONFIG_PROVISIONING_SERVICE_TASK_STACK": "6144",
            "CONFIG_MAIN_WEATHER_SERVER_BASE_URL":
                '"https://weather.example.com"',
            "CONFIG_MAIN_WEATHER_DEVICE_TOKEN": '"example-device-token"',
        }
        self.assertEqual(
            {key: assignments.get(key) for key in expected}, expected
        )

        provisioning_kconfig = (
            self.root / "layers/middleware/components/provisioning_service/Kconfig"
        ).read_text(encoding="utf-8")
        self.assertRegex(
            (
                self.root /
                "layers/middleware/components/connectivity_manager/Kconfig"
            ).read_text(encoding="utf-8"),
            r"config CONNECTIVITY_MANAGER_TASK_STACK\s+"
            r"int[^\n]*\s+default 6144\b\s+range 4096 8192\b",
        )
        self.assertRegex(
            provisioning_kconfig,
            r"config PROVISIONING_SERVICE_TASK_STACK\s+"
            r"int[^\n]*\s+default 6144\b",
        )
        self.assertRegex(
            provisioning_kconfig,
            r"config PROVISIONING_SERVICE_QUEUE_DEPTH\s+"
            r"int[^\n]*\s+default 8\b",
        )


if __name__ == "__main__":
    unittest.main()
