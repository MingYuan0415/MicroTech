#!/usr/bin/env python3
"""Tests for reproducible LVGL internal-RAM firmware profiles."""

import tempfile
import unittest
from pathlib import Path

import lvgl_ram_profiles as profiles


class LvglRamProfilesTest(unittest.TestCase):
    def setUp(self):
        self.project_root = profiles.project_root()

    def _write_materialized(self, output_dir, name, extra=None):
        profile = profiles.ALL_PROFILES[name]
        values = profiles.materialized_expected_values(profile)
        values["CONFIG_TEST_UNRELATED"] = "same"
        if extra is not None:
            values.update(extra)
        paths = profiles.profile_paths(output_dir, profile)
        paths.root.mkdir(parents=True, exist_ok=True)
        lines = []
        for key, value in sorted(values.items()):
            if value == "n":
                lines.append(f"# {key} is not set")
            else:
                lines.append(f"{key}={value}")
        paths.sdkconfig.write_text(
            "\n".join(lines) + "\n", encoding="utf-8"
        )
        paths.build.mkdir(parents=True, exist_ok=True)
        (paths.build / "microtech.bin").write_bytes(b"test firmware")
        profiles.display_profiles._write_profile_manifest(
            paths.manifest,
            profile,
            profiles.display_profiles.source_manifest(self.project_root),
        )
        paths.header.write_text(
            profiles.display_profiles.render_profile_header(
                profiles.benchmark_profile(self.project_root, profile)
            ),
            encoding="utf-8",
        )

    def test_candidate_order_and_reclaim_budget(self):
        self.assertEqual(profiles.PRIMARY_PROFILE_ORDER, ("B0", "C"))
        self.assertEqual(
            profiles.FALLBACK_PROFILE_ORDER,
            ("A", "B24", "B20", "B16"),
        )
        self.assertEqual(profiles.PROFILES["C"].theoretical_reclaim, 65536)
        self.assertEqual(profiles.PROFILES["B16"].theoretical_reclaim, 49152)
        self.assertNotIn("C_EXT", profiles.PROFILE_ORDER)
        self.assertEqual(
            profiles.DIAGNOSTIC_PROFILE_ORDER,
            ("C_EXT", "C_EXT_STRESS"),
        )
        self.assertEqual(
            profiles.DIAGNOSTIC_PROFILES["C_EXT_STRESS"].benchmark_profile,
            "c_ext_stress_1800.json",
        )

    def test_tracked_assets_are_ram_only(self):
        profiles.validate_assets(self.project_root)
        baseline = profiles.display_profiles.merge_configs(
            profiles.profile_defaults(
                self.project_root, profiles.PROFILES["B0"]
            )
        )
        synchronous = profiles.display_profiles.merge_configs(
            profiles.profile_defaults(
                self.project_root, profiles.PROFILES["C"]
            )
        )
        differences = profiles.display_profiles.differing_keys(
            baseline, synchronous
        )
        self.assertTrue(differences)
        self.assertEqual(differences - profiles.RAM_CONFIG_KEYS, set())

    def test_prepare_defaults_to_primary_profiles_and_isolates_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            output_dir = Path(temporary) / "lvgl-ram"
            commands = profiles.prepare(
                self.project_root, output_dir, False
            )
            self.assertEqual(len(commands), 2)
            self.assertIn("/b0/build", commands[0])
            self.assertIn("/c/build", commands[1])
            self.assertTrue(all("sdkconfig.defaults;" in item
                                for item in commands))
            self.assertTrue(all("DISPLAY_BENCHMARK_PROFILE_DIR=" in item
                                for item in commands))
            self.assertTrue(all(item.endswith("build size")
                                for item in commands))

    def test_materialized_matrix_accepts_only_ram_differences(self):
        with tempfile.TemporaryDirectory() as temporary:
            output_dir = Path(temporary)
            for name in profiles.PROFILE_ORDER:
                derived = None
                if name == "B0":
                    derived = {
                        "CONFIG_LV_DRAW_THREAD_PRIO": "3",
                        "CONFIG_LV_USE_FREERTOS_TASK_NOTIFY": "y",
                    }
                self._write_materialized(output_dir, name, derived)
            profiles.validate_matrix(
                self.project_root, output_dir, profiles.PROFILE_ORDER
            )

    def test_materialized_matrix_rejects_unrelated_drift(self):
        with tempfile.TemporaryDirectory() as temporary:
            output_dir = Path(temporary)
            self._write_materialized(output_dir, "B0")
            self._write_materialized(
                output_dir, "C", {"CONFIG_TEST_UNRELATED": "changed"}
            )
            with self.assertRaisesRegex(
                    profiles.ProfileError, "outside LVGL RAM"):
                profiles.validate_matrix(
                    self.project_root, output_dir, ("B0", "C")
                )

    def test_synchronous_profile_has_no_draw_stack(self):
        profile = profiles.PROFILES["C"]
        expected = profiles.expected_values(profile)
        self.assertEqual(expected["CONFIG_LV_OS_NONE"], "y")
        self.assertEqual(expected["CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT"], "1")
        self.assertNotIn("CONFIG_LV_DRAW_THREAD_STACK_SIZE", expected)
        self.assertEqual(
            expected["CONFIG_APP_MANAGER_LVGL_WORKER_STACK_SIZE"], "32768"
        )

    def test_external_nimble_diagnostic_adds_allocator_and_affinity(self):
        profile = profiles.DIAGNOSTIC_PROFILES["C_EXT"]
        expected = profiles.expected_values(profile)
        self.assertTrue(profile.os_none)
        self.assertTrue(profile.nimble_external)
        self.assertTrue(profile.task_affinity)
        self.assertEqual(
            expected["CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL"], "n"
        )
        self.assertEqual(
            expected["CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL"], "y"
        )
        self.assertEqual(
            expected["CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_DEFAULT"], "n"
        )
        materialized = profiles.materialized_expected_values(profile)
        self.assertEqual(
            materialized["CONFIG_NIMBLE_MEM_ALLOC_MODE_INTERNAL"], "n"
        )
        self.assertEqual(
            materialized["CONFIG_NIMBLE_MEM_ALLOC_MODE_EXTERNAL"], "y"
        )
        c_values = profiles.display_profiles.merge_configs(
            profiles.profile_defaults(
                self.project_root, profiles.PROFILES["C"]
            )
        )
        external_values = profiles.display_profiles.merge_configs(
            profiles.profile_defaults(self.project_root, profile)
        )
        differences = profiles.display_profiles.differing_keys(
            c_values, external_values
        )
        self.assertEqual(
            differences,
            profiles.NIMBLE_ALLOCATOR_CHANGED_CONFIG_KEYS |
            profiles.LEGACY_AFFINITY_CHANGED_CONFIG_KEYS,
        )
        self.assertEqual(
            expected["CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU0"], "y"
        )
        self.assertEqual(
            expected["CONFIG_APP_MANAGER_LVGL_WORKER_AFFINITY_CPU1"], "y"
        )
        self.assertEqual(
            materialized["CONFIG_MAIN_PROJECT_TASK_CORE_ID"], "0x0"
        )
        self.assertEqual(
            materialized["CONFIG_APP_MANAGER_LVGL_WORKER_CORE_ID"], "1"
        )
        self.assertTrue(
            profiles.AFFINITY_MATERIALIZED_OMITTED_KEYS.isdisjoint(
                materialized
            )
        )

    def test_non_external_profiles_restore_legacy_affinity(self):
        for profile in profiles.PROFILES.values():
            self.assertFalse(profile.task_affinity)
            expected = profiles.expected_values(profile)
            self.assertEqual(
                expected["CONFIG_MAIN_PROJECT_TASK_AFFINITY_NO_AFFINITY"],
                "y",
            )
            self.assertEqual(
                expected["CONFIG_APP_MANAGER_LVGL_WORKER_AFFINITY_NO_AFFINITY"],
                "y",
            )
            self.assertEqual(
                expected["CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL"], "y"
            )

    def test_external_stress_inherits_affinity(self):
        base = profiles.DIAGNOSTIC_PROFILES["C_EXT"]
        stress = profiles.DIAGNOSTIC_PROFILES["C_EXT_STRESS"]
        self.assertTrue(stress.task_affinity)
        self.assertEqual(
            profiles.expected_values(stress)[
                "CONFIG_APP_MANAGER_LVGL_WORKER_AFFINITY_CPU1"
            ],
            profiles.expected_values(base)[
                "CONFIG_APP_MANAGER_LVGL_WORKER_AFFINITY_CPU1"
            ],
        )

    def test_external_nimble_materialized_profile_is_isolated(self):
        with tempfile.TemporaryDirectory() as temporary:
            output_dir = Path(temporary)
            self._write_materialized(output_dir, "B0")
            self._write_materialized(output_dir, "C_EXT")
            profiles.validate_matrix(
                self.project_root, output_dir, ("B0", "C_EXT")
            )

    def test_external_affinity_materialization_rejects_drift(self):
        with tempfile.TemporaryDirectory() as temporary:
            output_dir = Path(temporary)
            self._write_materialized(output_dir, "B0")
            self._write_materialized(
                output_dir,
                "C_EXT",
                {"CONFIG_APP_MANAGER_LVGL_WORKER_CORE_ID": "0"},
            )
            with self.assertRaisesRegex(
                    profiles.ProfileError, "invalid settings"):
                profiles.validate_matrix(
                    self.project_root, output_dir, ("B0", "C_EXT")
                )

    def test_stress_materialized_profile_is_isolated(self):
        with tempfile.TemporaryDirectory() as temporary:
            output_dir = Path(temporary)
            self._write_materialized(output_dir, "C_EXT")
            self._write_materialized(output_dir, "C_EXT_STRESS")
            profiles.validate_matrix(
                self.project_root, output_dir, ("C_EXT", "C_EXT_STRESS")
            )

    def test_stress_materialized_profile_rejects_unrelated_drift(self):
        with tempfile.TemporaryDirectory() as temporary:
            output_dir = Path(temporary)
            self._write_materialized(output_dir, "C_EXT")
            self._write_materialized(
                output_dir,
                "C_EXT_STRESS",
                {"CONFIG_TEST_UNRELATED": "changed"},
            )
            with self.assertRaisesRegex(
                    profiles.ProfileError, "outside provisioning diagnostics"):
                profiles.validate_matrix(
                    self.project_root,
                    output_dir,
                    ("C_EXT", "C_EXT_STRESS"),
                )


if __name__ == "__main__":
    unittest.main()
