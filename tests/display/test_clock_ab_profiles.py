#!/usr/bin/env python3
"""Tests for reproducible display clock A/B firmware profiles."""

import tempfile
import unittest
from pathlib import Path

import clock_ab_profiles as profiles


class ClockAbProfilesTest(unittest.TestCase):
    def setUp(self):
        self.project_root = profiles.project_root()

    def _write_materialized(self, output_dir, name, extra=None):
        profile = profiles.PROFILES[name]
        values = profiles.expected_values(profile)
        values["CONFIG_TEST_UNRELATED"] = "same"
        if extra is not None:
            values.update(extra)
        path = profiles.profile_paths(output_dir, profile).sdkconfig
        path.parent.mkdir(parents=True, exist_ok=True)
        lines = []
        for key, value in sorted(values.items()):
            if value == "n":
                lines.append(f"# {key} is not set")
            else:
                lines.append(f"{key}={value}")
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        paths = profiles.profile_paths(output_dir, profile)
        paths.build.mkdir(parents=True, exist_ok=True)
        (paths.build / "microtech.bin").write_bytes(b"test firmware")
        profiles._write_profile_manifest(
            paths.manifest,
            profile,
            profiles.source_manifest(self.project_root),
        )

    def test_tracked_assets_compose_expected_profiles(self):
        profiles.validate_assets(self.project_root)

        e40 = profiles.merge_configs(profiles.profile_defaults(
            self.project_root, profiles.PROFILES["E40"]
        ))
        e80 = profiles.merge_configs(profiles.profile_defaults(
            self.project_root, profiles.PROFILES["E80"]
        ))
        self.assertEqual(
            profiles.differing_keys(e40, e80),
            profiles.CLOCK_CONFIG_KEYS - {
                "CONFIG_BSP_DISPLAY_SPI_CLOCK_HZ"
            },
        )

    def test_prepare_uses_distinct_paths_and_ordered_defaults(self):
        with tempfile.TemporaryDirectory() as temporary:
            output_dir = Path(temporary) / "clock-ab"
            commands = profiles.prepare(self.project_root, output_dir, False)

            self.assertEqual(len(commands), 4)
            selected = [profiles.PROFILES[name] for name in
                        profiles.CHARACTERIZATION_PROFILE_ORDER]
            sdkconfigs = {
                profiles.profile_paths(output_dir, profile).sdkconfig
                for profile in selected
            }
            builds = {
                profiles.profile_paths(output_dir, profile).build
                for profile in selected
            }
            self.assertEqual(len(sdkconfigs), 4)
            self.assertEqual(len(builds), 4)
            self.assertTrue(all(path.parent.is_dir() for path in sdkconfigs))
            self.assertTrue(all(
                profiles.profile_paths(output_dir, profile).manifest.is_file()
                for profile in selected
            ))
            self.assertTrue(all(str(path) in " ".join(commands)
                                for path in sdkconfigs))
            self.assertTrue(all(str(path) in " ".join(commands)
                                for path in builds))
            self.assertTrue(all("sdkconfig.defaults;" in command
                                for command in commands))

    def test_validation_rejects_a_different_source_manifest(self):
        with tempfile.TemporaryDirectory() as temporary:
            output_dir = Path(temporary)
            self._write_materialized(output_dir, "P40")
            self._write_materialized(output_dir, "P80")
            manifest = profiles.profile_paths(
                output_dir, profiles.PROFILES["P80"]
            ).manifest
            contents = manifest.read_text(encoding="utf-8")
            manifest.write_text(
                contents.replace(
                    '"source_fingerprint": "',
                    '"source_fingerprint": "changed-',
                    1,
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(
                    profiles.ProfileError, "source manifest"):
                profiles.validate_pair(output_dir, "P40", "P80")

    def test_prepare_rejects_unstamped_existing_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            output_dir = Path(temporary)
            paths = profiles.profile_paths(
                output_dir, profiles.PROFILES["E40"]
            )
            paths.build.mkdir(parents=True)

            with self.assertRaisesRegex(
                    profiles.ProfileError, "unstamped build artifacts"):
                profiles.prepare(
                    self.project_root, output_dir, False, ("E40",)
                )

    def test_long_run_profiles_are_independent_and_duration_only(self):
        self.assertEqual(
            profiles.benchmark_duration_range(self.project_root),
            (10, 28800),
        )
        with tempfile.TemporaryDirectory() as temporary:
            output_dir = Path(temporary)
            commands = profiles.prepare(
                self.project_root, output_dir, False,
                profiles.LONG_RUN_PROFILE_ORDER,
            )
            self.assertEqual(len(commands), 2)
            self.assertIn("e80-stress", commands[0])
            self.assertIn("e80-soak", commands[1])

            self._write_materialized(output_dir, "E80-STRESS")
            self._write_materialized(output_dir, "E80-SOAK")
            differences = profiles.validate_long_run_pair(output_dir)
            self.assertEqual(
                differences, {profiles.BENCHMARK_DURATION_KEY}
            )

    def test_materialized_pair_accepts_only_clock_differences(self):
        with tempfile.TemporaryDirectory() as temporary:
            output_dir = Path(temporary)
            self._write_materialized(output_dir, "E40")
            self._write_materialized(output_dir, "E80")

            differences = profiles.validate_pair(
                output_dir, "E40", "E80"
            )

            self.assertEqual(differences, profiles.CLOCK_CONFIG_KEYS)

    def test_materialized_pair_rejects_an_unrelated_difference(self):
        with tempfile.TemporaryDirectory() as temporary:
            output_dir = Path(temporary)
            self._write_materialized(output_dir, "P40")
            self._write_materialized(
                output_dir, "P80", {"CONFIG_TEST_UNRELATED": "changed"}
            )

            with self.assertRaisesRegex(
                    profiles.ProfileError, "not a clock-only pair"):
                profiles.validate_pair(output_dir, "P40", "P80")

    def test_materialized_profile_rejects_unsafe_transport(self):
        with tempfile.TemporaryDirectory() as temporary:
            output_dir = Path(temporary)
            self._write_materialized(
                output_dir, "E80",
                {"CONFIG_BSP_DISPLAY_NON_TE_PSRAM_DMA_DIRECT": "y"},
            )
            path = profiles.profile_paths(
                output_dir, profiles.PROFILES["E80"]
            ).sdkconfig

            with self.assertRaisesRegex(profiles.ProfileError, "invalid settings"):
                profiles.validate_materialized_profile(
                    path, profiles.PROFILES["E80"]
                )


if __name__ == "__main__":
    unittest.main()
