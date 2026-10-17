"""Independent configuration inputs and workload rendering regression tests."""
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import display_profiles as profiles


# Deliberately independent of the validator's expected-value table.
CONFIG = """CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=131072
CONFIG_LV_FREETYPE_CACHE_FT_GLYPH_CNT=64
CONFIG_APP_MANAGER_DISPLAY_DIAGNOSTICS=y
CONFIG_APP_MANAGER_LVGL_PARTIAL_BUFFER_HEIGHT=60
CONFIG_APP_MANAGER_PRESENTATION_SNAPSHOT_ANIMATION=y
CONFIG_MAIN_DISPLAY_BENCHMARK=y
CONFIG_LV_OS_NONE=y
CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=1
CONFIG_APP_MANAGER_LVGL_WORKER_STACK_SIZE=32768
CONFIG_APP_MANAGER_LVGL_WORKER_AFFINITY_CPU1=y
CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU0=y
CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y
"""


class DisplayProfilesTest(unittest.TestCase):
    def test_materialized_config_and_missing_values(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sdkconfig"
            path.write_text(CONFIG)
            values = profiles.utils.parse_config(path)
            profiles.validate_config(values)
            for key, value in (
                    ("CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL", "y"),
                    ("CONFIG_APP_MANAGER_LVGL_WORKER_AFFINITY_CPU1", "n"),
                    ("CONFIG_LV_DRAW_THREAD_STACK_SIZE", "32768")):
                with self.subTest(key=key), self.assertRaises(profiles.utils.ProfileError):
                    profiles.validate_config(values | {key: value})
            del values["CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT"]
            with self.assertRaisesRegex(profiles.utils.ProfileError, "missing"):
                profiles.validate_config(values)

    def test_prepare_isolated_campaigns_and_source_drift(self):
        root = profiles.utils.project_root()
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(
                profiles.utils, "source_manifest", return_value={"source_fingerprint": "one"}):
            output = Path(directory)
            commands = profiles.prepare(root, output, list(profiles.CAMPAIGNS), False)
            self.assertEqual(len(commands), 4)
            self.assertTrue(all(command.endswith(" build") for command in commands))
            header = (output / "ble-stress/display_benchmark_profile.h").read_text()
            self.assertIn(".ble_mode = DISPLAY_BENCHMARK_BLE_CONNECTED", header)
            self.assertIn(".stress_duration_sec = 1800U", header)
            self.assertIn(".app_workload = DISPLAY_BENCHMARK_APP_WORKLOAD_SYSTEM_ROUTES", header)
            with mock.patch.object(profiles.utils, "source_manifest", return_value={}):
                with self.assertRaisesRegex(profiles.utils.ProfileError, "sources changed"):
                    profiles.prepare(root, output, ["ble-stress"], False)

    def test_reject_malformed_workload(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "profile.json"
            path.write_text('{"mode": "stress"}')
            with self.assertRaisesRegex(profiles.utils.ProfileError, "schema mismatch"):
                profiles.utils.load_benchmark_profile(path)
