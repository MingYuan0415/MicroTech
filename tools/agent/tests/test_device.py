"""Device command construction and safety-gate tests."""

from __future__ import annotations

import argparse
import io
import json
import sys
import tempfile
import time
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve()
sys.path.insert(0, str(HERE.parents[1]))

import mt  # noqa: E402


class DeviceCommandTest(unittest.TestCase):
    def test_logs_parser_supports_snapshot_and_follow(self):
        parser = mt.build_parser()
        snapshot = parser.parse_args(['device', 'logs', '--snapshot'])
        follow = parser.parse_args(['device', 'logs', '--follow'])
        self.assertFalse(snapshot.follow)
        self.assertTrue(follow.follow)

    def test_monitor_timeout_status_is_normalized(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / 'monitor.log'
            log.write_text(
                "--- ERROR: Expect pattern 'boot id=' timed out after 3.0s\n",
                encoding='utf-8',
            )
            self.assertEqual(mt._normalize_monitor_rc(110, log), 124)
            self.assertEqual(mt._normalize_monitor_rc(110, Path(tmp) /
                                                       'missing.log'), 110)

    def test_boot_banner_is_parsed(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / 'monitor.log'
            log.write_text(
                "I (123) main: boot id=0123456789abcdef reason=POWERON "
                "version=1.2.3 elf=79116cba6e55ecf2 partition=factory\n",
                encoding='utf-8',
            )
            banner = mt._boot_banner(log)
        self.assertEqual(banner['id'], '0123456789abcdef')
        self.assertEqual(banner['reason'], 'POWERON')
        self.assertEqual(banner['version'], '1.2.3')
        self.assertEqual(banner['elf'], '79116cba6e55ecf2')
        self.assertEqual(banner['partition'], 'factory')

    def test_monitor_command_echo_does_not_satisfy_expect(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / 'monitor.log'
            log.write_text(
                "--- Command: 'expect --timeout 1 boot id='\n"
                "I (123) main: ready\n",
                encoding='utf-8',
            )
            lines = mt._device_monitor_lines(0, log, 'boot id=')
        self.assertTrue(lines[0].startswith('[FAIL]'))

    def test_monitor_connection_error_is_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / 'monitor.log'
            log.write_text(
                "Could not configure port: (25, 'Inappropriate ioctl for device')\n"
                "--- ERROR: Connection to /dev/null failed. Available ports:\n",
                encoding='utf-8',
            )
            self.assertEqual(mt._normalize_monitor_rc(0, log), 1)
            lines = mt._device_monitor_lines(0, log, None)
        self.assertTrue(lines[0].startswith('[FAIL]'))

    def test_app_connection_error_is_not_monitor_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / 'monitor.log'
            log.write_text(
                "2026-11-15 15:00:00 I (1) wifi: Connection to example.com failed\n",
                encoding='utf-8',
            )
            self.assertEqual(mt._normalize_monitor_rc(0, log), 0)
            lines = mt._device_monitor_lines(0, log, None)
        self.assertTrue(lines[0].startswith('[PASS]'))

    def test_monitor_expect_failure_returns_nonzero(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            elf = root / 'microtech.elf'
            elf.write_bytes(b'elf')
            log = root / 'monitor.log'
            log.write_text(
                "--- Command: 'expect --timeout 1 boot id='\n"
                "2026-11-15 15:00:00 I (1) main: ready\n",
                encoding='utf-8',
            )
            args = argparse.Namespace(port='/dev/null', expect='boot id=',
                                      timeout=1, reset=False)
            with mock.patch.object(mt, '_device_port', return_value='/dev/null'), \
                    mock.patch.object(mt, '_device_build_meta',
                                      return_value={'app_elf': str(elf),
                                                    'target': 'esp32s3'}), \
                    mock.patch.object(mt, '_new_log', return_value=log), \
                    mock.patch.object(mt, '_run_with_input',
                                      return_value=(0, 0.0, 0)), \
                    redirect_stdout(io.StringIO()):
                rc = mt._cmd_device_monitor(args)
        self.assertEqual(rc, 1)

    def test_probe_requires_mac_chip_and_matching_target(self):
        good = {'target': 'esp32s3',
                'probe': {'mac': 'aa:bb:cc:dd:ee:ff',
                          'chip': 'ESP32-S3'}}
        self.assertTrue(mt._device_probe_ok(good))
        self.assertFalse(mt._device_probe_ok(
            {'target': 'esp32s3', 'probe': {'error': 'MAC probe failed'}}))
        self.assertFalse(mt._device_probe_ok(
            {'target': 'esp32p4',
             'probe': {'mac': 'aa:bb:cc:dd:ee:ff', 'chip': 'ESP32-S3'}}))

    def test_monitor_command_keeps_no_reset_by_default(self):
        with tempfile.TemporaryDirectory() as tmp:
            elf = Path(tmp) / "microtech.elf"
            elf.write_bytes(b"elf")
            command = mt._device_monitor_command("/dev/ttyACM0", False, elf)
        self.assertIn("--no-reset", command)
        self.assertIn("--decode-panic", command)
        self.assertIn(str(elf), command)

    def test_monitor_command_adds_reset_only_when_requested(self):
        with tempfile.TemporaryDirectory() as tmp:
            elf = Path(tmp) / "microtech.elf"
            elf.write_bytes(b"elf")
            command = mt._device_monitor_command("/dev/ttyACM0", True, elf)
        self.assertNotIn("--no-reset", command)

    def test_flash_refuses_without_explicit_yes(self):
        args = argparse.Namespace(yes=False, port="/dev/ttyACM0", monitor=False,
                                  timeout=10)
        output = io.StringIO()
        with redirect_stdout(output):
            rc = mt._cmd_device_flash(args)
        self.assertEqual(rc, 2)
        self.assertIn("requires --yes", output.getvalue())

    def test_partition_info_reads_coredump_row(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config = root / "build/config"
            config.mkdir(parents=True)
            (config / "sdkconfig.h").write_text(
                '#define CONFIG_PARTITION_TABLE_CUSTOM_FILENAME '
                '"partitions_test.csv"\n', encoding="utf-8")
            (root / "partitions_test.csv").write_text(
                "nvs,data,nvs,0x9000,24K,\n"
                "coredump,data,coredump,0xff0000,64K,\n",
                encoding="utf-8")
            with mock.patch.object(mt, "ROOT", root):
                info = mt._device_partition_info()
        self.assertEqual(info.get("offset"), "0xff0000")
        self.assertEqual(info.get("size"), "64K")

    def test_crash_uses_resolved_build_dir(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            elf = root / "app.elf"
            elf.write_bytes(b"elf")
            log = root / "crash.log"
            meta = {"build_dir": str(root / "alt-build"),
                    "app_elf": str(elf)}
            with mock.patch.object(mt, "_device_build_meta", return_value=meta), \
                    mock.patch.object(mt, "_device_coredump_enabled",
                                      return_value=True), \
                    mock.patch.object(mt, "_device_port", return_value="/dev/null"), \
                    mock.patch.object(mt, "_new_log", return_value=log), \
                    mock.patch.object(mt, "_run", return_value=(0, 0.1, 0)) as run, \
                    mock.patch.object(mt, "_append_stats"), \
                    redirect_stdout(io.StringIO()):
                rc = mt._cmd_device_crash(argparse.Namespace(
                    port="/dev/null", save_core=None, timeout=10))
        self.assertEqual(rc, 0)
        self.assertEqual(
            run.call_args.args[0],
            ["idf.py", "-B", str(root / "alt-build"),
             "coredump-info", "-p", "/dev/null"],
        )


class DoctorTest(unittest.TestCase):
    def test_doctor_json_contract(self):
        checks = [mt._doctor_check("demo", "pass", "ok")]
        with tempfile.TemporaryDirectory() as tmp, \
                mock.patch.object(mt, "_doctor_checks", return_value=checks), \
                mock.patch.object(mt, "_append_stats"), \
                mock.patch.object(mt, "STATS_PATH", Path(tmp) / "stats.jsonl"):
            output = io.StringIO()
            with redirect_stdout(output):
                rc = mt._cmd_doctor(argparse.Namespace(json=True))
        self.assertEqual(rc, 0)
        self.assertIn('"ok": true', output.getvalue())

    def test_doctor_required_failure_returns_one(self):
        checks = [mt._doctor_check("demo", "fail", "missing")]
        with mock.patch.object(mt, "_doctor_checks", return_value=checks), \
                mock.patch.object(mt, "_append_stats"):
            with redirect_stdout(io.StringIO()):
                rc = mt._cmd_doctor(argparse.Namespace(json=True))
        self.assertEqual(rc, 1)

    def test_doctor_failure_records_failure_metadata(self):
        checks = [mt._doctor_check("demo", "fail", "missing")]
        with tempfile.TemporaryDirectory() as tmp:
            stats = Path(tmp) / "stats.jsonl"
            with mock.patch.object(mt, "_doctor_checks", return_value=checks), \
                    mock.patch.object(mt, "STATS_PATH", stats):
                with redirect_stdout(io.StringIO()):
                    rc = mt._cmd_doctor(argparse.Namespace(json=True))
            records = [
                json.loads(line)
                for line in stats.read_text(encoding="utf-8").splitlines()
                if line.strip()
            ]
        self.assertEqual(rc, 1)
        self.assertEqual(records[-1]["failure_label"], "doctor: demo: missing")
        self.assertIsNotNone(records[-1]["failure_signature"])
        self.assertEqual(records[-1]["repeat_count"], 1)


class DeviceFailureRegressionTest(unittest.TestCase):
    def test_monitor_expect_strips_timestamp_before_matching(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "monitor.log"
            log.write_text(
                "2026-11-15 15:00:00 boot id=0123456789abcdef reason=USB\n",
                encoding="utf-8",
            )
            lines = mt._device_monitor_lines(0, log, r"^boot id=")
        self.assertTrue(lines[0].startswith("[PASS]"))

    def test_monitor_expect_matches_message_body(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "monitor.log"
            log.write_text(
                "I (1182) main: boot id=0123456789abcdef reason=USB\n",
                encoding="utf-8",
            )
            lines = mt._device_monitor_lines(0, log, r"^boot id=")
        self.assertTrue(lines[0].startswith("[PASS]"))

    def test_logs_without_serial_output_is_not_a_pass(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "monitor.log"
            log.write_text("# command: monitor\n# cwd: /repo\n\n",
                           encoding="utf-8")
            lines = mt._device_monitor_lines(0, log, None, require_output=True)
        self.assertTrue(lines[0].startswith("[FAIL]"))

    def test_logs_rejects_nonpositive_seconds(self):
        for seconds in (0, -1):
            with self.subTest(seconds=seconds):
                with self.assertRaises(mt.UsageError):
                    mt._cmd_device_logs(argparse.Namespace(
                        port="/dev/null", follow=False, seconds=seconds,
                        timeout=None, reset=False, grep=None))

    def test_flash_failure_never_prints_a_pass_headline(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "flash.log"
            before = {
                "target": "esp32s3", "build_ready": True,
                "probe": {"mac": "aa:bb:cc:dd:ee:ff", "chip": "ESP32-S3"},
            }
            after = dict(before)
            after["probe"] = {"mac": "aa:bb:cc:dd:ee:00",
                              "chip": "ESP32-S3"}
            output = io.StringIO()
            with mock.patch.object(mt, "_device_port", return_value="/dev/null"), \
                    mock.patch.object(mt, "_new_log", return_value=log), \
                    mock.patch.object(mt, "_device_status_data",
                                      side_effect=[before, before, after]), \
                    mock.patch.object(mt, "_run", return_value=(0, 0.1, 0)), \
                    mock.patch.object(mt, "_append_stats"), \
                    redirect_stdout(output):
                rc = mt._cmd_device_flash(argparse.Namespace(
                    yes=True, port="/dev/null", monitor=False, timeout=10))
        self.assertEqual(rc, 1)
        self.assertTrue(output.getvalue().startswith("[FAIL] device flash"))
        self.assertNotIn("[PASS] device flash", output.getvalue())

    def test_flash_uses_resolved_build_dir(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "flash.log"
            before = {
                "target": "esp32s3", "build_ready": True,
                "build_dir": str(Path(tmp) / "alt-build"),
                "probe": {"mac": "aa:bb:cc:dd:ee:ff", "chip": "ESP32-S3"},
            }
            with mock.patch.object(mt, "_device_port", return_value="/dev/null"), \
                    mock.patch.object(mt, "_new_log", return_value=log), \
                    mock.patch.object(mt, "_device_status_data",
                                      side_effect=[before, before, before]), \
                    mock.patch.object(mt, "_run", return_value=(0, 0.1, 0)) as run, \
                    mock.patch.object(mt, "_append_stats"), \
                    redirect_stdout(io.StringIO()):
                rc = mt._cmd_device_flash(argparse.Namespace(
                    yes=True, port="/dev/null", monitor=False, timeout=10))
        self.assertEqual(rc, 0)
        self.assertEqual(
            run.call_args.args[0],
            ["idf.py", "-B", str(Path(tmp) / "alt-build"),
             "-p", "/dev/null", "flash"],
        )

    def test_flash_refuses_incomplete_local_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "flash.log"
            output = io.StringIO()
            with mock.patch.object(mt, "_device_port", return_value="/dev/null"), \
                    mock.patch.object(mt, "_new_log", return_value=log), \
                    mock.patch.object(mt, "_device_status_data",
                                      return_value={"build_ready": False,
                                                    "build_dir": str(tmp)}), \
                    mock.patch.object(mt, "_run") as run, \
                    mock.patch.object(mt, "_append_stats"), \
                    redirect_stdout(output):
                rc = mt._cmd_device_flash(argparse.Namespace(
                    yes=True, port="/dev/null", monitor=False, timeout=10))
        self.assertEqual(rc, 1)
        self.assertIn("local build is incomplete", output.getvalue())
        run.assert_not_called()

    def test_boot_banner_check_rejects_unknown_partition_and_version(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "boot.log"
            log.write_text(
                "boot id=abc reason=USB version=old elf=79116cba6 "
                "partition=unknown\n", encoding="utf-8")
            lines, ok = mt._boot_banner_check(
                log, {"elf_sha256": "79116cba6e55", "app_version": "new"})
        self.assertFalse(ok)
        self.assertTrue(any("partition" in line for line in lines))
        self.assertTrue(any("version" in line for line in lines))

    def test_run_with_input_timeout_covers_blocked_stdin(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "monitor.log"
            rc, _, _ = mt._run_with_input(
                [sys.executable, "-c", "import time; time.sleep(30)"],
                log, 1, "x" * (1024 * 1024))
        self.assertEqual(rc, 124)

    def test_run_streaming_timeout_returns_124(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "monitor.log"
            rc, _, _ = mt._run_streaming(
                [sys.executable, "-c", "import time; time.sleep(30)"],
                log, 1)
        self.assertEqual(rc, 124)

    def test_build_meta_uses_project_description_build_dir(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build = root / "build"
            build.mkdir()
            alt = root / "alt-build"
            alt.mkdir()
            (alt / "mt_product_profile.h").write_text(
                '#define MT_PRODUCT_NAME "s3_reference"\n',
                encoding="utf-8")
            (alt / "app.bin").write_bytes(b"bin")
            (alt / "app.elf").write_bytes(b"elf")
            (build / "project_description.json").write_text(
                '{"build_dir": "%s", "target": "esp32p4", '
                '"project_version": "1.0", "app_bin": "app.bin", '
                '"app_elf": "app.elf"}' % alt, encoding="utf-8")
            with mock.patch.object(mt, "ROOT", root), \
                    mock.patch.object(mt.subprocess, "run",
                                      return_value=mock.Mock(
                                          returncode=0, stdout="")):
                meta = mt._device_build_meta()
        self.assertEqual(meta["build_dir"], str(alt))
        self.assertEqual(meta["app_bin"], str(alt / "app.bin"))
        self.assertEqual(meta["app_elf"], str(alt / "app.elf"))


class BuildCoredumpTest(unittest.TestCase):
    def _run_build(self, coredump):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "build").mkdir()
            log = root / "build.log"
            log.write_text("", encoding="utf-8")
            with mock.patch.object(mt, "ROOT", root), \
                    mock.patch.object(mt.shutil, "which",
                                      return_value="/usr/bin/idf.py"), \
                    mock.patch.dict(mt.os.environ,
                                    {"IDF_PATH": str(root)}), \
                    mock.patch.object(mt, "_new_log", return_value=log), \
                    mock.patch.object(mt, "_run",
                                      return_value=(0, 1.0, 0)) as run, \
                    mock.patch.object(mt, "_build_summary",
                                      return_value=["[PASS] build idf"]), \
                    mock.patch.object(mt, "_build_context_entry",
                                      return_value={"schema": 1}), \
                    mock.patch.object(mt, "_device_coredump_enabled",
                                      return_value=coredump):
                result = mt._build_idf(False, 10, coredump=coredump)
        argv = run.call_args.args[0]
        return result[0], argv, root

    def test_coredump_build_uses_isolated_sdkconfig(self):
        rc, argv, root = self._run_build(True)
        self.assertEqual(rc, 0)
        self.assertEqual(argv[:3], ["idf.py", "-B", str(root / "build")])
        self.assertIn("SDKCONFIG=" + str(root / "build" / "sdkconfig.coredump"), argv)
        self.assertIn("SDKCONFIG_DEFAULTS=sdkconfig.defaults;"
                      "sdkconfig.coredump.defaults", argv)

    def test_plain_build_uses_repository_sdkconfig(self):
        rc, argv, root = self._run_build(False)
        self.assertEqual(rc, 0)
        self.assertIn("SDKCONFIG=" + str(root / "sdkconfig"), argv)
        self.assertNotIn("sdkconfig.coredump", " ".join(argv))


class DeviceLogsRegressionTest(unittest.TestCase):
    def test_explicit_timeout_overrides_snapshot_default(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "logs.log"
            elf = Path(tmp) / "app.elf"
            elf.write_bytes(b"elf")
            with mock.patch.object(mt, "_device_port",
                                   return_value="/dev/null"), \
                    mock.patch.object(mt, "_new_log", return_value=log), \
                    mock.patch.object(mt, "_device_build_meta",
                                      return_value={"app_elf": str(elf)}), \
                    mock.patch.object(mt, "_run_with_input",
                                      return_value=(0, 0.1, 0)) as run, \
                    mock.patch.object(mt, "_device_monitor_lines",
                                      return_value=["[PASS] device logs"]), \
                    mock.patch.object(mt, "_finish", return_value=0):
                rc = mt._cmd_device_logs(argparse.Namespace(
                    port="/dev/null", follow=False, seconds=5, timeout=7,
                    reset=False, grep=None))
        self.assertEqual(rc, 0)
        self.assertEqual(run.call_args.args[2], 7)

    def test_grep_filters_body_without_duplicate_copy(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "logs.log"
            log.write_text("noise\nMATCH target\nnoise 2\n", encoding="utf-8")
            elf = Path(tmp) / "app.elf"
            elf.write_bytes(b"elf")
            captured = []

            def fake_finish(lines, *_args):
                captured.extend(lines)
                return 0

            with mock.patch.object(mt, "_device_port",
                                   return_value="/dev/null"), \
                    mock.patch.object(mt, "_new_log", return_value=log), \
                    mock.patch.object(mt, "_device_build_meta",
                                      return_value={"app_elf": str(elf)}), \
                    mock.patch.object(mt, "_run_with_input",
                                      return_value=(0, 0.1, 0)), \
                    mock.patch.object(mt, "_device_monitor_lines",
                                      return_value=["[PASS] device logs"]), \
                    mock.patch.object(mt, "_finish", side_effect=fake_finish):
                rc = mt._cmd_device_logs(argparse.Namespace(
                    port="/dev/null", follow=False, seconds=5, timeout=7,
                    reset=False, grep="MATCH"))
        self.assertEqual(rc, 0)
        self.assertEqual(captured, ["[PASS] device logs", "  MATCH target"])

    def test_empty_snapshot_is_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "logs.log"
            log.write_text("", encoding="utf-8")
            elf = Path(tmp) / "app.elf"
            elf.write_bytes(b"elf")
            output = io.StringIO()
            with mock.patch.object(mt, "_device_port",
                                   return_value="/dev/null"), \
                    mock.patch.object(mt, "_new_log", return_value=log), \
                    mock.patch.object(mt, "_device_build_meta",
                                      return_value={"app_elf": str(elf)}), \
                    mock.patch.object(mt, "_run_with_input",
                                      return_value=(0, 0.1, 0)), \
                    mock.patch.object(mt, "_append_stats"), \
                    redirect_stdout(output):
                rc = mt._cmd_device_logs(argparse.Namespace(
                    port="/dev/null", follow=False, seconds=1, timeout=2,
                    reset=False, grep=None))
        self.assertEqual(rc, 1)
        self.assertTrue(output.getvalue().startswith(
            "[FAIL] device logs: no serial output captured"))
        self.assertNotIn("[PASS]", output.getvalue())

    def test_run_streaming_keeps_partial_line_and_times_out(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "monitor.log"
            started = time.monotonic()
            with redirect_stdout(io.StringIO()):
                rc, _, _ = mt._run_streaming(
                    [sys.executable, "-c",
                     "import sys,time; sys.stdout.write('partial'); "
                     "sys.stdout.flush(); time.sleep(30)"],
                    log, 1)
            elapsed = time.monotonic() - started
            body = log.read_text(encoding="utf-8")
        self.assertEqual(rc, 124)
        self.assertLess(elapsed, 5)
        self.assertIn("partial", body)


class FlashMonitorRegressionTest(unittest.TestCase):
    def test_nonpositive_monitor_timeout_is_rejected_before_flash(self):
        with mock.patch.object(mt, "_device_port") as port, \
                mock.patch.object(mt, "_device_status_data") as status, \
                mock.patch.object(mt, "_run") as run:
            with self.assertRaisesRegex(
                    mt.UsageError,
                    "--monitor-timeout must be greater than zero"):
                mt._cmd_device_flash(argparse.Namespace(
                    yes=True, port="/dev/null", monitor=True,
                    monitor_timeout=0, timeout=10))
        port.assert_not_called()
        status.assert_not_called()
        run.assert_not_called()

    def test_monitor_timeout_reports_verify_failure_after_write(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            log = root / "flash.log"
            elf = root / "app.elf"
            elf.write_bytes(b"elf")
            before = {
                "target": "esp32s3", "build_ready": True,
                "build_dir": str(root),
                "probe": {"mac": "aa:bb:cc:dd:ee:ff", "chip": "ESP32-S3"},
            }
            output = io.StringIO()
            with mock.patch.object(mt, "_device_port",
                                   return_value="/dev/null"), \
                    mock.patch.object(mt, "_new_log", return_value=log), \
                    mock.patch.object(mt, "_device_status_data",
                                      side_effect=[before, before, before]), \
                    mock.patch.object(mt, "_run",
                                      return_value=(0, 0.1, 0)), \
                    mock.patch.object(mt, "_device_build_meta",
                                      return_value={"app_elf": str(elf)}), \
                    mock.patch.object(mt, "_device_monitor_command",
                                      return_value=["monitor"]), \
                    mock.patch.object(mt, "_run_with_input",
                                      return_value=(124, 0.2, 0)) as run, \
                    mock.patch.object(mt, "_normalize_monitor_rc",
                                      return_value=124), \
                    mock.patch.object(mt, "_append_stats"), \
                    redirect_stdout(output):
                rc = mt._cmd_device_flash(argparse.Namespace(
                    yes=True, port="/dev/null", monitor=True,
                    monitor_timeout=1, timeout=10))
        self.assertEqual(rc, 1)
        self.assertTrue(output.getvalue().startswith(
            "[FAIL] device flash verify (image written)"))
        self.assertIn("[FAIL] device monitor rc=124", output.getvalue())
        self.assertEqual(run.call_args.args[2], 11)
        self.assertEqual(run.call_args.args[3],
                         "expect --timeout 1 boot id=\nexit\n")


if __name__ == "__main__":
    unittest.main()
