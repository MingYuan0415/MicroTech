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
        self.assertEqual(len(symbols), 49, sorted(symbols))

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

    def test_device_link_contract_source_is_authoritative(self) -> None:
        contract = self.root / "contracts/device_link"
        protocol = yaml.safe_load(
            (contract / "protocol.yaml").read_text(encoding="utf-8")
        )
        self.assertEqual(
            (contract / "VERSION").read_text(encoding="utf-8").strip(),
            "1.0.0",
        )
        self.assertEqual(protocol["profile"], {
            "name": "device-link/v1",
            "schema_format": "fixed-binary/1",
            "version": "1.0.0",
            "release_state": "freeze_candidate",
        })
        self.assertEqual(
            protocol["protocol"]["transport"]["preferred_att_mtu"], 498
        )
        self.assertEqual(
            protocol["protocol"]["transport"]["maximum_att_value_bytes"], 495
        )
        security = protocol["protocol"]["security"]
        self.assertEqual(security["transport"], "ble_le_secure_connections")
        self.assertTrue(security["sc_only"])
        self.assertTrue(security["mitm"])
        self.assertTrue(security["bonding"])
        self.assertEqual(security["max_bonds"], 1)
        self.assertEqual(security["io_capability"], "display_yes_no")
        self.assertEqual(security["association_model"], "numeric_comparison")
        self.assertEqual(security["encryption_key_bytes"], 16)
        self.assertEqual(security["bond_replacement"], "local_clear_then_pair")
        self.assertNotIn("bond_replacement_candidate", security)
        characteristics = protocol["protocol"]["gatt"]["characteristics"]
        self.assertEqual(characteristics["command_rx"]["properties"], ["write"])
        self.assertEqual(characteristics["server_tx"]["properties"], ["indicate"])
        for characteristic in characteristics.values():
            self.assertTrue(characteristic["encrypted"])
            self.assertTrue(characteristic["authenticated"])
        self.assertTrue(characteristics["server_tx"]["cccd_write_encrypted"])
        self.assertTrue(
            characteristics["server_tx"]["cccd_write_authenticated"]
        )
        self.assertEqual(
            protocol["protocol"]["att_errors"][
                "insufficient_authentication"
            ],
            0x05,
        )
        self.assertEqual(
            protocol["protocol"]["transport"]["operation_id_wire"], "u32"
        )
        transport = protocol["protocol"]["transport"]
        self.assertEqual(transport["application_error_opcode"], 0x80)
        self.assertNotIn("l2cap_pdu_bytes", transport)
        self.assertFalse(transport["operation_id_reuse_within_boot"])
        self.assertEqual(transport["operation_id_exhausted_status"], "INTERNAL")
        self.assertNotIn("recovery_requires_full_mtu", transport["low_mtu"])
        self.assertEqual(
            {command["name"]: command["id"] for command in protocol["commands"]}
            ["GET_OPERATION"],
            8,
        )
        self.assertEqual(
            {command["name"]: command["id"] for command in protocol["commands"]}
            ["ACK_OPERATION"],
            9,
        )
        self.assertNotIn("admission", protocol["wire_rules"])
        self.assertNotIn("scan", protocol["wire_rules"])
        self.assertIn(
            "SCAN",
            protocol["wire_rules"]["operation_result"]["failure_matrix"],
        )
        vectors = yaml.safe_load(
            (contract / "vectors/golden.json").read_text(encoding="utf-8")
        )
        self.assertEqual(vectors["format_version"], 4)

        gitmodules = (self.root / ".gitmodules").read_text(encoding="utf-8")
        self.assertIn("path = contracts/device_link", gitmodules)
        self.assertIn(
            "url = https://github.com/MingYuan0415/mt-device-link-contract.git",
            gitmodules,
        )

    def test_device_link_contract_lock_matches_worktree(self) -> None:
        lock = self.root / "contracts/device_link.lock"
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
        self.assertEqual(lock_values, {
            "lock_format": "2",
            "contract_commit": contract_head,
            "profile": "device-link/v1",
            "schema_format": "fixed-binary/1",
            "schema_digest": digest,
            "release_state": "freeze_candidate",
            "release_tag": "none",
            "pin_state": "working_tree_candidate",
        })

    def test_device_link_contract_checks_and_current_status_are_declared(
            self) -> None:
        contract = self.root / "contracts/device_link"
        workflow = (
            contract / ".github/workflows/contract.yml"
        ).read_text(encoding="utf-8")
        status = (
            self.root / "contracts/device_link/README.md"
        ).read_text(encoding="utf-8")
        wifi_policy = (
            self.root /
            "layers/middleware/components/wifi_service/README.md"
        ).read_text(encoding="utf-8")
        normalized_policy = " ".join(wifi_policy.split())
        self.assertIn("tooling.check", workflow)
        self.assertIn("unittest discover", workflow)
        self.assertIn("device-link/v1", status)
        self.assertIn("freeze candidate", status)
        self.assertIn("non-normative", wifi_policy)
        self.assertIn("Pending policy target", wifi_policy)
        self.assertIn("does not claim Device Link v1 policy conformance",
                      normalized_policy)
        root_readme = (self.root / "README.md").read_text(encoding="utf-8")
        legacy_client = (
            self.root / "tests/connectivity/device_link_client/README.md"
        ).read_text(encoding="utf-8")
        self.assertNotIn("Security2", root_readme)
        self.assertNotIn("contracts/provisioning", legacy_client)
        self.assertIn("not a `device-link/v1` conformance client", legacy_client)


if __name__ == "__main__":
    unittest.main()
