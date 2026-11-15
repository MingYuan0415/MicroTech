"""Parser and output-contract tests for the agent wrapper."""

from __future__ import annotations

import argparse
import io
import sys
import tempfile
import time
import unittest
import xml.etree.ElementTree as ET
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve()
sys.path.insert(0, str(HERE.parents[1]))

import mt  # noqa: E402

GCC_ERROR = """\
[1/3] Building C object CMakeFiles/demo.dir/demo.c.o
In file included from /repo/include/demo.h:12,
                 from /repo/src/demo.c:4:
/repo/src/demo.c:42:5: error: 'UNKNOWN' undeclared (first use in this function)
   42 |     UNKNOWN = 1;
      |     ^~~~~~~
[2/3] Building C object CMakeFiles/demo.dir/other.c.o
"""

LINK_ERROR = """\
[3/3] Linking C executable demo
/usr/bin/ld: CMakeFiles/demo.dir/demo.c.o: in function `main':
demo.c:(.text+0x12): undefined reference to `missing_symbol'
collect2: error: ld returned 1 exit status
ninja: build stopped: subcommand failed.
"""

JUNIT = """\
<?xml version="1.0" encoding="UTF-8"?>
<testsuites>
  <testsuite name="demo" tests="2" failures="1" errors="0">
    <testcase name="passes" time="0.01"/>
    <testcase name="fails" time="0.02">
      <failure message="assertion failed">expected 1, got 2
at demo_test.c:42</failure>
    </testcase>
  </testsuite>
</testsuites>
"""


class BuildParserTest(unittest.TestCase):
    def _summary(self, text: str, rc: int) -> list[str]:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "build.log"
            path.write_text(text, encoding="utf-8")
            return mt._build_summary("demo", rc, 1.0, path)

    def test_success_counts_warnings(self):
        lines = self._summary("demo.c:1: warning: unused\n", 0)
        self.assertTrue(lines[0].startswith("[PASS] build demo"))
        self.assertIn("1 warnings", lines[0])

    def test_compile_error_keeps_location_and_context(self):
        lines = self._summary(GCC_ERROR, 1)
        text = "\n".join(lines)
        self.assertTrue(lines[0].startswith("[FAIL] build demo"))
        self.assertIn("demo.c:42:5: error:", text)
        self.assertIn("In file included from", text)

    def test_linker_error_is_kept(self):
        lines = self._summary(LINK_ERROR, 1)
        text = "\n".join(lines)
        self.assertIn("undefined reference", text)
        self.assertIn("ninja: build stopped", text)

    def test_long_line_is_truncated_with_pointer(self):
        long_line = "src/demo.c:1:1: error: " + ("x" * 9000)
        lines = mt._cap_lines(self._summary(long_line, 1))
        self.assertLess(max(len(line) for line in lines), 5000)
        self.assertIn("see log", "\n".join(lines))

    def test_build_idf_accepts_coredump_overlay(self):
        args = mt.build_parser().parse_args(
            ["build", "idf", "--coredump"])
        self.assertTrue(args.coredump)


class SimBuildGateTest(unittest.TestCase):
    def test_failure_stops_after_the_failing_configuration(self):
        calls = []

        def fake_build(spec, clean, timeout, log, append):
            calls.append(spec.name)
            return (0 if len(calls) == 1 else 1), 0.0

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            log = root / "gate.log"
            with mock.patch.object(mt, "ROOT", root), \
                    mock.patch.object(mt, "_require_sim_env", lambda: None), \
                    mock.patch.object(mt, "_new_log", lambda _: log), \
                    mock.patch.object(mt, "_build_sim_configuration",
                                      fake_build):
                rc, lines, _, _ = mt._build_sim_gate(False, 1)

        self.assertEqual(rc, 1)
        self.assertEqual(calls, ["s3-fake", "s3-real"])
        self.assertIn("s3-real", lines[0])


class TestResultTest(unittest.TestCase):
    def test_junit_counts_and_failure_detail(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            xml = build / "result.xml"
            xml.write_text(JUNIT, encoding="utf-8")
            total, failed, skipped = mt._junit_results(xml)
            self.assertEqual((total, skipped), (2, 0))
            self.assertEqual(failed[0][0], "fails")
            lines = mt._test_summary("demo", 1, 0.1, build, xml)
            text = "\n".join(lines)
            self.assertTrue(lines[0].startswith("[FAIL] test demo"))
            self.assertIn("FAILED fails", text)
            self.assertIn("expected 1, got 2", text)

    def test_no_tests_is_a_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            xml = build / "empty.xml"
            xml.write_text(
                "<testsuites><testsuite name='x' tests='0'/></testsuites>",
                encoding="utf-8",
            )
            lines = mt._test_summary("demo", 8, 0.1, build, xml)
            self.assertTrue(lines[0].startswith("[FAIL] test demo"))
            self.assertIn("no tests found", lines[0])

    def test_success_summary(self):
        with tempfile.TemporaryDirectory() as tmp:
            xml = Path(tmp) / "ok.xml"
            root = ET.Element("testsuites")
            suite = ET.SubElement(root, "testsuite", name="ok")
            for name in ("a", "b"):
                ET.SubElement(suite, "testcase", name=name)
            ET.ElementTree(root).write(xml, encoding="utf-8")
            lines = mt._test_summary("demo", 0, 0.2, Path(tmp), xml)
            self.assertEqual(lines, ["[PASS] test demo 2/2 (0.2s)"])


class OutputContractTest(unittest.TestCase):
    def test_log_body_strips_short_monitor_header_without_eating_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'monitor.log'
            path.write_text(
                '# command: monitor\n# cwd: /repo\n\nFIRST SERIAL LINE\n',
                encoding='utf-8',
            )
            self.assertEqual(mt._read_body(path).splitlines()[0],
                             'FIRST SERIAL LINE')

    def test_cap_lines_is_bounded(self):
        lines = [f"line {index}" for index in range(100)]
        capped = mt._cap_lines(lines)
        self.assertLessEqual(len(capped), mt.SUMMARY_MAX_LINES + 1)
        self.assertTrue(capped[-1].startswith("... [summary truncated"))

    def test_python_summary_keeps_group_lines(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "py.log"
            path.write_text(
                "test_a ... ok\nRan 1 test\nOK\n"
                "[PASS] docs duration=0.1s\n",
                encoding="utf-8",
            )
            lines = mt._py_summary(["docs"], 0, 0.1, path)
            self.assertIn("[PASS] docs duration=0.1s", lines)
            self.assertTrue(lines[-1].startswith("[PASS] py docs"))

    def test_ui_summary_uses_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = Path(tmp) / "report.json"
            report.write_text(
                '{"checked": 4, "snapshots": 2, "failures": []}',
                encoding="utf-8",
            )
            lines = mt._ui_summary(0, 0.1, report, Path(tmp) / "shots",
                                   Path(tmp) / "ui.log")
            self.assertTrue(lines[0].startswith("[PASS] ui 4 states"))
            self.assertIn("snapshots=2", lines[0])
            report.write_text(
                '{"checked": 4, "snapshots": 2, "failures": [],'
                ' "build_context_error": "missing"}', encoding="utf-8")
            lines = mt._ui_summary(0, 0.1, report, Path(tmp) / "shots",
                                   Path(tmp) / "ui.log")
            self.assertTrue(lines[0].startswith("[FAIL] ui"))

    def test_run_timeout_returns_124_and_keeps_partial_log(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "timeout.log"
            rc, _, _ = mt._run(
                [sys.executable, "-c",
                 "import time; print('partial', flush=True); time.sleep(30)"],
                path,
                timeout=1,
            )
            self.assertEqual(rc, 124)
            self.assertIn("partial", mt._read_log(path))


class ErrorClassifierTest(unittest.TestCase):
    def test_compile_link_and_cmake_categories(self):
        self.assertEqual(mt._classify_build_error(GCC_ERROR), "compile")
        self.assertEqual(mt._classify_build_error(LINK_ERROR), "link")
        self.assertEqual(
            mt._classify_build_error("CMake Error at CMakeLists.txt:1:"),
            "cmake")

    def test_ninja_error_is_not_classified_as_compile(self):
        self.assertEqual(
            mt._classify_build_error("ninja: error: build stopped"), "ninja")

    def test_unknown_error_is_explicit(self):
        self.assertEqual(mt._classify_build_error("something odd"), "unknown")


class InspectCommandTest(unittest.TestCase):
    def test_inspect_reports_category_and_first_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "build.log"
            log.write_text(GCC_ERROR, encoding="utf-8")
            output = io.StringIO()
            with mock.patch.object(mt, "LOG_ROOT", Path(tmp)), \
                    redirect_stdout(output):
                rc = mt._cmd_inspect(argparse.Namespace(
                    log=str(log), grep=None, context=0, json=False))
        self.assertEqual(rc, 0)
        self.assertIn("category: compile", output.getvalue())
        self.assertIn("demo.c:42:5: error:", output.getvalue())

    def test_inspect_grep_keeps_diagnostic_context(self):
        log_text = "In file included from demo.h:1\ndemo.c:42:5: error: bad\n"
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "build.log"
            log.write_text(log_text, encoding="utf-8")
            output = io.StringIO()
            with mock.patch.object(mt, "LOG_ROOT", Path(tmp)), \
                    redirect_stdout(output):
                rc = mt._cmd_inspect(argparse.Namespace(
                    log=str(log), grep="error:", context=0, json=False))
        self.assertEqual(rc, 0)
        self.assertIn("In file included from demo.h:1", output.getvalue())


if __name__ == "__main__":
    unittest.main()
