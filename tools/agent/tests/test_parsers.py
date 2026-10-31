"""Parser and output-contract tests for the agent wrapper."""

from __future__ import annotations

import sys
import tempfile
import time
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

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
                '{"checked": 4, "visual_checked": 2, "failures": []}',
                encoding="utf-8",
            )
            lines = mt._ui_summary(0, 0.1, report, Path(tmp) / "shots",
                                   Path(tmp) / "ui.log")
            self.assertTrue(lines[0].startswith("[PASS] ui 4 states"))

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


if __name__ == "__main__":
    unittest.main()
