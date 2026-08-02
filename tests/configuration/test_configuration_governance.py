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
            for token in DEPRECATED_CONFIG_TOKENS:
                if token in text:
                    failures.append(f"{relative}: deprecated {token}")
        self.assertEqual(failures, [], "\n".join(failures))

    def test_connectivity_defaults(self) -> None:
        defaults = (self.root / "sdkconfig.defaults").read_text(encoding="utf-8")
        assignments = dict(re.findall(
            r"^(CONFIG_[A-Z0-9_]+)=(.+)$", defaults, re.MULTILINE
        ))
        expected = {
            "CONFIG_BT_ENABLED": "y",
            "CONFIG_BT_NIMBLE_ENABLED": "y",
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
            "CONFIG_ESP_WIFI_NVS_ENABLED": "y",
            "CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_2": "y",
            "CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_PATCH_VERSION": "y",
            "CONFIG_LWIP_SNTP_UPDATE_DELAY": "3600000",
            "CONFIG_LV_USE_QRCODE": "y",
            "CONFIG_PROVISIONING_SERVICE_TASK_STACK": "6144",
        }
        self.assertEqual(
            {key: assignments.get(key) for key in expected}, expected
        )

        provisioning_kconfig = (
            self.root / "layers/middleware/components/provisioning_service/Kconfig"
        ).read_text(encoding="utf-8")
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
