"""Failure signature, repeat detection, and stats tests."""

from __future__ import annotations

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


class FailureSubjectTest(unittest.TestCase):
    def test_ui_line_has_priority(self):
        lines = [
            "[FAIL] ui settings/wifi",
            "  - settings__wifi__scan_list: expected 1 page, got 0",
            "ERROR: scenario failed",
        ]
        self.assertEqual(
            mt._failure_subject(lines),
            "settings__wifi__scan_list: expected 1 page, got 0",
        )

    def test_compiler_error_is_extracted(self):
        lines = [
            "[1/3] Building C object demo.c.o",
            "/repo/src/demo.c:42:5: error: 'UNKNOWN' undeclared",
            "ninja: build stopped",
        ]
        self.assertEqual(
            mt._failure_subject(lines),
            "/repo/src/demo.c:42:5: error: 'UNKNOWN' undeclared",
        )

    def test_python_failure_is_extracted(self):
        lines = [
            "Traceback (most recent call last):",
            '  File "/repo/test_demo.py", line 4, in test_demo',
            "AssertionError: expected 1, got 2",
        ]
        self.assertEqual(
            mt._failure_subject(lines), "AssertionError: expected 1, got 2"
        )

    def test_fail_headline_is_fallback(self):
        self.assertEqual(
            mt._failure_subject(["[FAIL] test apps: no tests found"]),
            "[FAIL] test apps: no tests found",
        )


class FailureSignatureTest(unittest.TestCase):
    def test_normalization_ignores_paths_numbers_and_addresses(self):
        first = mt._normalize_failure(
            "/repo/src/demo.c:42:5: error: boom at 0x1234 after 12 ms",
            keep_numbers=False,
        )
        second = mt._normalize_failure(
            "/tmp/other/src/demo.c:87:9: error: boom at 0xffff after 99 ms",
            keep_numbers=False,
        )
        self.assertEqual(first, second)

    def test_different_messages_are_distinct(self):
        first = mt._failure_signature(["AssertionError: boom"])
        second = mt._failure_signature(["AssertionError: bang"])
        self.assertIsNotNone(first)
        self.assertIsNotNone(second)
        self.assertNotEqual(first, second)

    def test_empty_input_has_no_signature(self):
        self.assertIsNone(mt._failure_signature([]))


class FailureStreakTest(unittest.TestCase):
    def _entry(self, timestamp: str, signature: str | None,
               rc: int = 1) -> dict[str, object]:
        return {
            "timestamp": timestamp,
            "command": "test",
            "suite": "apps",
            "rc": rc,
            "failure_signature": signature,
        }

    def test_counts_consecutive_matching_failures(self):
        with tempfile.TemporaryDirectory() as tmp:
            stats = Path(tmp) / "stats.jsonl"
            stats.write_text(
                "\n".join(json.dumps(entry) for entry in [
                    self._entry("2026-11-01T10:00:00", "abc"),
                    self._entry("2026-11-01T10:01:00", "abc"),
                ]) + "\n",
                encoding="utf-8",
            )
            with mock.patch.object(mt, "STATS_PATH", stats):
                count, previous = mt._failure_streak("test", "apps", "abc")
        self.assertEqual(count, 3)
        self.assertIsNotNone(previous)
        self.assertEqual(previous["timestamp"], "2026-11-01T10:01:00")

    def test_success_or_different_failure_resets(self):
        with tempfile.TemporaryDirectory() as tmp:
            stats = Path(tmp) / "stats.jsonl"
            stats.write_text(
                "\n".join(json.dumps(entry) for entry in [
                    self._entry("2026-11-01T10:00:00", "abc"),
                    self._entry("2026-11-01T10:01:00", None, rc=0),
                ]) + "\n",
                encoding="utf-8",
            )
            with mock.patch.object(mt, "STATS_PATH", stats):
                count, previous = mt._failure_streak("test", "apps", "abc")
        self.assertEqual(count, 1)
        self.assertIsNone(previous)


class FinishTest(unittest.TestCase):
    def _finish(self, root: Path, lines: list[str], rc: int = 1,
                full: bool = False):
        log = root / "run.log"
        log.write_text("complete body\n", encoding="utf-8")
        stats = root / "stats.jsonl"
        with mock.patch.object(mt, "ROOT", root), \
                mock.patch.object(mt, "STATS_PATH", stats):
            output = io.StringIO()
            with redirect_stdout(output):
                result = mt._finish(
                    lines, log, "test", "apps", rc, time.monotonic(), full,
                )
            records = [
                json.loads(line)
                for line in stats.read_text(encoding="utf-8").splitlines()
                if line.strip()
            ]
        return result, output.getvalue(), records

    def test_first_failure_records_signature_without_loop_hint(self):
        with tempfile.TemporaryDirectory() as tmp:
            result, output, records = self._finish(
                Path(tmp),
                ["[FAIL] test apps", "AssertionError: expected 1, got 2"],
            )
        self.assertEqual(result, 1)
        self.assertNotIn("[LOOP]", output)
        self.assertEqual(records[-1]["repeat_count"], 1)
        self.assertIsNotNone(records[-1]["failure_signature"])
        self.assertIn("AssertionError", str(records[-1]["failure_label"]))

    def test_second_matching_failure_gets_loop_hint(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            lines = ["[FAIL] test apps", "AssertionError: expected 1, got 2"]
            _, _, first = self._finish(root, lines)
            _, output, records = self._finish(root, lines)
        self.assertIn("[LOOP] same failure x2 for test apps", output)
        self.assertEqual(records[-1]["repeat_count"], 2)
        self.assertEqual(
            records[-1]["failure_signature"],
            first[-1]["failure_signature"],
        )

    def test_success_resets_streak(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._finish(
                root,
                ["[FAIL] test apps", "AssertionError: expected 1, got 2"],
            )
            _, output, records = self._finish(
                root, ["[PASS] test apps"], rc=0,
            )
        self.assertNotIn("[LOOP]", output)
        self.assertEqual(records[-1]["repeat_count"], 0)
        self.assertIsNone(records[-1]["failure_signature"])
        self.assertIsNone(records[-1]["failure_label"])

    def test_timeout_keeps_timeout_line_and_records_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            result, output, records = self._finish(
                Path(tmp), ["[FAIL] test apps"], rc=124,
            )
        self.assertEqual(result, 124)
        self.assertIn("[TIMEOUT] test apps exceeded its time limit", output)
        self.assertEqual(records[-1]["repeat_count"], 1)

    def test_full_mode_puts_loop_hint_at_top(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            lines = ["[FAIL] test apps", "AssertionError: expected 1, got 2"]
            self._finish(root, lines)
            _, output, _ = self._finish(root, lines, full=True)
        self.assertTrue(output.startswith("[LOOP] same failure x2"))
        self.assertIn("complete body", output)

    def test_loop_hint_follows_last_fail_headline(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            lines = [
                "[PASS] agent duration=0.1s",
                "[FAIL] docs duration=0.2s",
                "[FAIL] py agent docs (0.3s)",
                "AssertionError: expected 1, got 2",
            ]
            self._finish(root, lines)
            _, output, _ = self._finish(root, lines)
        rendered = output.splitlines()
        self.assertEqual(rendered[0], "[PASS] agent duration=0.1s")
        self.assertEqual(rendered[1], "[FAIL] docs duration=0.2s")
        self.assertEqual(rendered[2], "[FAIL] py agent docs (0.3s)")
        self.assertTrue(rendered[3].startswith("[LOOP] same failure x2"))


if __name__ == "__main__":
    unittest.main()
