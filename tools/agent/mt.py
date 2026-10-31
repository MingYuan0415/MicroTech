#!/usr/bin/env python3
"""Token-efficient command wrapper for repository agent workflows.

The wrapper is a presentation layer only: the wrapped command decides pass or
fail, and its complete output is kept under `.cache/agent/logs/`. The summary
printed to the agent is bounded, while the log path always remains available.

Usage:
    python3 tools/agent/mt.py list
    python3 tools/agent/mt.py build apps
    python3 tools/agent/mt.py test apps
    python3 tools/agent/mt.py py configuration resources docs
    python3 tools/agent/mt.py fmt --check
    python3 tools/agent/mt.py git status
    python3 tools/agent/mt.py sim --scenario agent_contract
    python3 tools/agent/mt.py ui --pages settings/wifi
"""

from __future__ import annotations

import argparse
import contextlib
import fcntl
import importlib.util
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from datetime import datetime
from functools import lru_cache
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD_ROOT = Path(os.environ.get("MT_BUILD_ROOT", ROOT / "build" / "agent"))
LOG_ROOT = Path(os.environ.get("MT_LOG_ROOT", ROOT / ".cache" / "agent" / "logs"))
STATS_PATH = Path(os.environ.get("MT_STATS_PATH", ROOT / ".cache" / "agent" / "stats.jsonl"))

SUMMARY_MAX_LINES = 40
SUMMARY_MAX_BYTES = 4096
DIAGNOSTIC_LINE_MAX = 2048
PATCH_MAX_LINES = 200

ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")
ERROR_RE = re.compile(
    r"(?:^|\s)(?:fatal error|error):|^FAILED:|undefined reference to|"
    r"CMake Error|ninja: error|collect2: error|ld: cannot find",
    re.IGNORECASE,
)
WARNING_RE = re.compile(r"(?:^|\s)warning:", re.IGNORECASE)
CONTEXT_RE = re.compile(
    r"In file included from|In function|instantiated from|required from|note:|^\s+from\s"
)


@dataclass(frozen=True)
class Suite:
    alias: str
    source: Path
    sanitizer_var: str | None
    tags: tuple[str, ...] = ()


class UsageError(RuntimeError):
    """An environment or usage problem that the agent can fix."""


@lru_cache(maxsize=1)
def _git_head() -> str:
    try:
        head = subprocess.check_output(
            ["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"],
            text=True,
            stderr=subprocess.DEVNULL,
            timeout=5,
        ).strip()
        dirty = subprocess.run(
            ["git", "-C", str(ROOT), "status", "--porcelain", "--untracked-files=no"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=5,
            check=False,
        ).stdout.strip()
        return f"{head}{'+dirty' if dirty else ''}"
    except (OSError, subprocess.SubprocessError):
        return "unknown"


@lru_cache(maxsize=8)
def _tool_version(tool: str) -> str:
    try:
        result = subprocess.run(
            [tool, "--version"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=5,
            check=False,
        )
        first = (result.stdout or "").splitlines()
        return first[0].strip() if first else "unknown"
    except (OSError, subprocess.SubprocessError):
        return "unknown"


def _sanitizer_var(cmake: Path) -> str | None:
    text = cmake.read_text(encoding="utf-8", errors="replace")
    matches = set(
        re.findall(
            r'set\(\s*([A-Z][A-Z0-9_]*_SANITIZER)\s+"(?:none|address|thread)"',
            text,
        )
    )
    return matches.pop() if len(matches) == 1 else None


def discover_suites() -> dict[str, Suite]:
    suites: dict[str, Suite] = {}
    special = (
        ("main", ROOT / "main/tests/host", ()),
        ("bsp", ROOT / "layers/bsp/tests/host", ()),
        ("apps", ROOT / "layers/apps/tests/host", ()),
        ("app-core", ROOT / "layers/app_manager/app_core/tests/host", ()),
        ("integration", ROOT / "tests/integration", ()),
        ("connectivity", ROOT / "tests/connectivity", ("needs-idf",)),
    )
    for alias, source, tags in special:
        cmake = source / "CMakeLists.txt"
        if cmake.is_file():
            suites[alias] = Suite(alias, source, _sanitizer_var(cmake), tags)

    components = ROOT / "layers/middleware/components"
    for source in sorted(components.glob("*/tests/host")):
        cmake = source / "CMakeLists.txt"
        if not cmake.is_file():
            continue
        alias = source.parents[1].name.replace("_", "-")
        suites[alias] = Suite(alias, source, _sanitizer_var(cmake))

    return dict(sorted(suites.items()))


SUITES = discover_suites()


def _rel(path: Path) -> str:
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def _strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text).replace("\r\n", "\n").replace("\r", "\n")


def _cap_line(line: str, limit: int = DIAGNOSTIC_LINE_MAX) -> str:
    if len(line) <= limit:
        return line
    return line[:limit] + f" ... [truncated {len(line) - limit} chars, see log]"


def _cap_lines(lines: list[str], max_lines: int = SUMMARY_MAX_LINES,
               max_bytes: int = SUMMARY_MAX_BYTES) -> list[str]:
    out: list[str] = []
    used = 0
    for line in lines:
        line = _cap_line(line)
        cost = len(line.encode("utf-8")) + 1
        if len(out) >= max_lines or used + cost > max_bytes:
            if not out:
                out.append(line[:max_bytes])
            out.append(f"... [summary truncated, see log]")
            break
        out.append(line)
        used += cost
    return out


def _timestamp() -> str:
    return datetime.now().strftime("%Y%m%d-%H%M%S-%f")


def _base_env() -> dict[str, str]:
    env = os.environ.copy()
    env.update(
        {
            "NO_COLOR": "1",
            "GIT_PAGER": "cat",
            "PAGER": "cat",
            "PYTHONUNBUFFERED": "1",
            "PYTHONIOENCODING": "utf-8",
            "LC_ALL": "C.UTF-8",
        }
    )
    return env


def _mtime(path: Path) -> float:
    try:
        return path.stat().st_mtime
    except OSError:
        return 0.0


def _prune_run_dirs(pattern: str, keep: int) -> None:
    dirs = sorted(
        (path for path in LOG_ROOT.glob(pattern) if path.is_dir()),
        key=_mtime,
    )
    now = time.time()
    for old in dirs[:-keep]:
        if now - _mtime(old) > 60:
            shutil.rmtree(old, ignore_errors=True)


def _new_log(name: str) -> Path:
    LOG_ROOT.mkdir(parents=True, exist_ok=True)
    logs = sorted(LOG_ROOT.glob("*.log"), key=_mtime)
    now = time.time()
    for old in logs[:-40]:
        if now - _mtime(old) > 60:
            with contextlib.suppress(OSError):
                old.unlink()
    return LOG_ROOT / f"{_timestamp()}-{name}.log"


def _append_stats(payload: dict[str, object]) -> None:
    STATS_PATH.parent.mkdir(parents=True, exist_ok=True)
    with STATS_PATH.open("a", encoding="utf-8") as fh:
        with contextlib.suppress(OSError):
            os.chmod(STATS_PATH, 0o600)
        fh.write(json.dumps(payload, ensure_ascii=False) + "\n")


@contextlib.contextmanager
def _exclusive_lock(path: Path):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(path, os.O_CREAT | os.O_RDWR, 0o600)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print(f"[WAIT] waiting for {_rel(path)}", flush=True)
            fcntl.flock(fd, fcntl.LOCK_EX)
        yield
    finally:
        fcntl.flock(fd, fcntl.LOCK_UN)
        os.close(fd)


def _run(cmd: list[str], log_path: Path, timeout: int,
         cwd: Path | None = None, append: bool = False) -> tuple[int, float, int]:
    """Run a command with output redirected to a log file.

    Returns (returncode, seconds, raw_log_bytes). The process gets its own
    session so a timeout can terminate a whole build/scenario tree.
    """
    LOG_ROOT.mkdir(parents=True, exist_ok=True)
    mode = "a" if append else "w"
    started = time.monotonic()
    with log_path.open(mode, encoding="utf-8", errors="replace") as fh:
        with contextlib.suppress(OSError):
            os.chmod(log_path, 0o600)
        if not append:
            fh.write(f"# command: {' '.join(cmd)}\n")
            fh.write(f"# cwd: {cwd or ROOT}\n")
            fh.write(f"# git: {_git_head()}\n")
            fh.write(f"# cmake: {_tool_version('cmake')}; ninja: {_tool_version('ninja')}\n")
            fh.write("\n")
            fh.flush()
        proc = None
        try:
            proc = subprocess.Popen(
                cmd,
                cwd=str(cwd or ROOT),
                stdout=fh,
                stderr=subprocess.STDOUT,
                stdin=subprocess.DEVNULL,
                text=True,
                env=_base_env(),
                start_new_session=True,
            )
        except OSError as error:
            raise UsageError(f"cannot execute {cmd[0]}: {error}") from error
        try:
            rc = proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            rc = 124
            _terminate(proc)
            fh.write(f"\n# timeout after {timeout}s\n")
        except KeyboardInterrupt:
            _terminate(proc)
            raise
    elapsed = time.monotonic() - started
    return rc, elapsed, log_path.stat().st_size


def _terminate(proc: subprocess.Popen) -> None:
    with contextlib.suppress(ProcessLookupError):
        os.killpg(proc.pid, signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        with contextlib.suppress(ProcessLookupError):
            os.killpg(proc.pid, signal.SIGKILL)
        with contextlib.suppress(subprocess.TimeoutExpired):
            proc.wait(timeout=5)


def _read_log(path: Path) -> str:
    try:
        return _strip_ansi(path.read_text(encoding="utf-8", errors="replace"))
    except OSError:
        return ""


def _log_body(text: str) -> str:
    lines = text.splitlines()
    if lines and lines[0].startswith("# command:"):
        del lines[0:4]
    while lines and lines[0].strip() == "":
        del lines[0]
    return "\n".join(lines)


def _read_body(path: Path) -> str:
    return _log_body(_read_log(path))


def _finish(lines: list[str], log_path: Path, command: str, suite: str,
            rc: int, started: float, full: bool = False) -> int:
    if rc == 124:
        lines = lines + [f"[TIMEOUT] {command} {suite} exceeded its time limit"]
    if full:
        body = _read_log(log_path).rstrip().splitlines()
        lines = body + [f"log: {_rel(log_path)}"]
    else:
        lines = _cap_lines(lines)
        if log_path:
            lines.append(f"log: {_rel(log_path)}")
    print("\n".join(lines))
    _append_stats(
        {
            "timestamp": datetime.now().isoformat(timespec="seconds"),
            "command": command,
            "suite": suite,
            "rc": rc,
            "seconds": round(time.monotonic() - started, 3),
            "raw_bytes": log_path.stat().st_size if log_path.exists() else 0,
            "emitted_bytes": len(("\n".join(lines)).encode("utf-8")),
            "log": _rel(log_path),
        }
    )
    return rc


def _warning_lines(text: str) -> list[str]:
    seen: list[str] = []
    for line in text.splitlines():
        if WARNING_RE.search(line):
            line = line.strip()
            if line not in seen:
                seen.append(line)
    return seen


def _diagnostic_blocks(text: str) -> list[str]:
    lines = text.splitlines()
    ranges: list[tuple[int, int]] = []
    for index, line in enumerate(lines):
        if ERROR_RE.search(line):
            start = index
            while start > 0 and index - start < 2 and CONTEXT_RE.search(lines[start - 1]):
                start -= 1
            ranges.append((start, min(len(lines), index + 3)))
    merged: list[tuple[int, int]] = []
    for start, end in ranges:
        if merged and start <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        else:
            merged.append((start, end))
    out: list[str] = []
    for start, end in merged:
        out.extend(lines[start:end])
    return out


def _tail(text: str, count: int = 40) -> list[str]:
    return text.splitlines()[-count:]


def _build_summary(alias: str, rc: int, seconds: float,
                   log_path: Path) -> list[str]:
    text = _read_body(log_path)
    warnings = _warning_lines(text)
    if rc == 0:
        lines = [
            f"[PASS] build {alias} ({len(warnings)} warnings, {seconds:.1f}s)"
        ]
        lines.extend(f"  {line[:300]}" for line in warnings[:5])
        if len(warnings) > 5:
            lines.append(f"  ... {len(warnings) - 5} more warnings in log")
        return lines
    lines = [f"[FAIL] build {alias} rc={rc} ({seconds:.1f}s)"]
    blocks = _diagnostic_blocks(text)
    lines.extend(blocks if blocks else _tail(text))
    return lines


def _build_dir(alias: str, profile: str) -> Path:
    return BUILD_ROOT / "host" / alias / profile


def _validate_profile(suite: Suite, profile: str) -> None:
    if profile not in ("none", "address", "thread"):
        raise UsageError(f"unsupported sanitizer profile: {profile}")
    if profile != "none" and not suite.sanitizer_var:
        raise UsageError(
            f"suite {suite.alias} does not expose a *_SANITIZER CMake option"
        )


def _build_host(suite: Suite, profile: str, clean: bool,
                timeout: int, full: bool) -> tuple[int, list[str], Path, float]:
    _validate_profile(suite, profile)
    if "needs-idf" in suite.tags and not os.environ.get("IDF_PATH"):
        raise UsageError(
            f"{suite.alias} needs IDF_PATH; source the ESP-IDF export script"
        )
    build = _build_dir(suite.alias, profile)
    if clean:
        shutil.rmtree(build, ignore_errors=True)
    log = _new_log(f"build-{suite.alias}-{profile}")
    build.parent.mkdir(parents=True, exist_ok=True)
    with _exclusive_lock(build.parent / ".lock"):
        configure = [
            "cmake", "-S", str(suite.source), "-B", str(build), "-G", "Ninja",
        ]
        if suite.sanitizer_var:
            configure.append(f"-D{suite.sanitizer_var}={profile}")
        rc, seconds, _ = _run(configure, log, timeout)
        if rc != 0:
            lines = _build_summary(suite.alias, rc, seconds, log)
            return rc, lines, log, seconds
        rc, build_seconds, _ = _run(
            ["cmake", "--build", str(build), "--", "--quiet"],
            log,
            timeout,
            append=True,
        )
    return (
        rc,
        _build_summary(suite.alias, rc, seconds + build_seconds, log),
        log,
        seconds + build_seconds,
    )


def _build_sim(clean: bool, timeout: int) -> tuple[int, list[str], Path, float]:
    if not os.environ.get("IDF_PATH"):
        raise UsageError("IDF_PATH is unset; source the ESP-IDF export script first")
    if not (ROOT / "build/config/sdkconfig.h").is_file():
        raise UsageError(
            "build/config/sdkconfig.h is missing; run `idf.py build` once"
        )
    build = ROOT / "build/sim"
    if clean:
        shutil.rmtree(build, ignore_errors=True)
    log = _new_log("build-sim")
    with _exclusive_lock(build / ".sim-ci-build.lock"):
        rc, seconds, _ = _run(
            ["cmake", "-S", str(ROOT / "sim"), "-B", str(build), "-G", "Ninja"],
            log,
            timeout,
        )
        if rc != 0:
            return rc, _build_summary("sim", rc, seconds, log), log, seconds
        rc, build_seconds, _ = _run(
            ["cmake", "--build", str(build), "--", "--quiet"],
            log,
            timeout,
            append=True,
        )
    return (
        rc,
        _build_summary("sim", rc, seconds + build_seconds, log),
        log,
        seconds + build_seconds,
    )


def _build_idf(clean: bool, timeout: int) -> tuple[int, list[str], Path, float]:
    if not shutil.which("idf.py") or not os.environ.get("IDF_PATH"):
        raise UsageError("idf.py/IDF_PATH is unavailable; source the export script first")
    log = _new_log("build-idf")
    if clean:
        rc, seconds, _ = _run(["idf.py", "fullclean"], log, timeout)
        if rc != 0:
            return rc, _build_summary("idf", rc, seconds, log), log, seconds
    rc, seconds, _ = _run(["idf.py", "build"], log, max(timeout, 1800))
    return rc, _build_summary("idf", rc, seconds, log), log, seconds


def _junit_results(path: Path) -> tuple[int, list[tuple[str, str]], int]:
    total = 0
    failed: list[tuple[str, str]] = []
    skipped = 0
    root = ET.parse(path).getroot()
    for case in root.iter("testcase"):
        total += 1
        name = case.get("name") or "unnamed"
        problem = case.find("failure")
        if problem is None:
            problem = case.find("error")
        if problem is not None:
            detail = (problem.text or problem.get("message") or "").strip()
            failed.append((name, detail))
        if case.find("skipped") is not None:
            skipped += 1
    return total, failed, skipped


def _load_last_test_log(build: Path) -> str:
    path = build / "Testing" / "Temporary" / "LastTest.log"
    return _read_log(path) if path.is_file() else ""


def _test_summary(alias: str, rc: int, seconds: float, build: Path,
                  xml: Path) -> list[str]:
    total = 0
    failed: list[tuple[str, str]] = []
    skipped = 0
    if xml.is_file():
        try:
            total, failed, skipped = _junit_results(xml)
        except ET.ParseError:
            total = 0
    if total == 0:
        text = _load_last_test_log(build)
        lines = [f"[FAIL] test {alias} no tests found (rc={rc}, {seconds:.1f}s)"]
        lines.extend(_tail(text, 20))
        return lines
    if rc == 0 and not failed:
        extra = f", {skipped} skipped" if skipped else ""
        return [f"[PASS] test {alias} {total}/{total} ({seconds:.1f}s{extra})"]
    if not failed:
        return [
            f"[FAIL] test {alias} rc={rc} ({seconds:.1f}s)",
            *_tail(_load_last_test_log(build), 30),
        ]
    lines = [
        f"[FAIL] test {alias} {total - len(failed)}/{total} "
        f"({seconds:.1f}s)"
    ]
    for name, detail in failed[:5]:
        lines.append(f"FAILED {name}")
        detail_lines = [line for line in detail.splitlines() if line.strip()]
        lines.extend("  " + line[:300] for line in detail_lines[:12])
        if len(detail_lines) > 12:
            lines.append(f"  ... {len(detail_lines) - 12} more lines in log")
    if len(failed) > 5:
        lines.append(f"... {len(failed) - 5} more failing tests in log")
    return lines


def _test_host(suite: Suite, profile: str, clean: bool, timeout: int,
               filter_re: str | None, repeat: str | None,
               full: bool) -> tuple[int, list[str], Path, float]:
    rc, build_lines, build_log, build_seconds = _build_host(
        suite, profile, clean, timeout, full
    )
    if rc != 0:
        return rc, build_lines, build_log, build_seconds
    build = _build_dir(suite.alias, profile)
    log = _new_log(f"test-{suite.alias}-{profile}")
    xml = log.with_suffix(".xml")
    cmd = [
        "ctest", "--test-dir", str(build), "--no-tests=error",
        "--output-on-failure", "--output-junit", str(xml),
    ]
    if filter_re:
        cmd.extend(["-R", filter_re])
    if repeat:
        cmd.extend(["--repeat", repeat])
    with _exclusive_lock(build.parent / ".lock"):
        rc, seconds, _ = _run(cmd, log, timeout)
    summary = _test_summary(suite.alias, rc, seconds, build, xml)
    if build_lines and "0 warnings" not in build_lines[0]:
        summary.insert(1, build_lines[0])
    return (
        rc,
        summary,
        log,
        build_seconds + seconds,
    )


def _py_summary(groups: list[str], rc: int, seconds: float,
                log: Path) -> list[str]:
    text = _read_body(log)
    group_lines = [
        line for line in text.splitlines()
        if line.startswith("[PASS] ") or line.startswith("[FAIL] ")
    ]
    if rc == 0:
        return group_lines + [
            f"[PASS] py {' '.join(groups) if groups else 'all'} ({seconds:.1f}s)"
        ]
    lines = group_lines + [
        f"[FAIL] py {' '.join(groups) if groups else 'all'} ({seconds:.1f}s)"
    ]
    start = None
    for index, line in enumerate(text.splitlines()):
        if line.startswith("FAIL:") or line.startswith("ERROR:"):
            start = index
            break
    lines.extend(text.splitlines()[start:] if start is not None else _tail(text))
    return lines


def _fmt_repos() -> list[Path]:
    repos = [ROOT]
    try:
        output = subprocess.check_output(
            ["git", "-C", str(ROOT), "config", "--file", ".gitmodules",
             "--get-regexp", r"^submodule\..*\.path$"],
            text=True,
            stderr=subprocess.DEVNULL,
            timeout=5,
        )
    except (OSError, subprocess.SubprocessError):
        return repos
    for line in output.splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        path = ROOT / parts[1]
        if (path / ".git").exists():
            repos.append(path)
    return repos


def _excluded(path: str) -> bool:
    return (
        "managed_components/" in path
        or "XPowersLib/" in path
        or Path(path).name.startswith("mmap_generate_")
    )


def _changed_sources(repo: Path, use_all: bool) -> list[str]:
    if use_all:
        output = subprocess.check_output(
            ["git", "-C", str(repo), "ls-files", "*.c", "*.h"],
            text=True,
            timeout=10,
        )
        files = [line for line in output.splitlines() if line]
    else:
        output = subprocess.check_output(
            ["git", "-C", str(repo), "status", "--porcelain",
             "--untracked-files=all"],
            text=True,
            timeout=10,
        )
        files = []
        for line in output.splitlines():
            if len(line) < 4:
                continue
            path = line[3:]
            if " -> " in path:
                path = path.split(" -> ", 1)[1]
            files.append(path.strip('"'))
    return [
        path for path in files
        if path.endswith((".c", ".h")) and not _excluded(path)
        and (repo / path).is_file()
    ]


def _astyle_cmd(files: list[str], check: bool) -> list[str]:
    cmd = [
        "astyle", "--style=allman", "--indent=spaces=4",
        "--indent-preproc-block", "--pad-oper", "--pad-header",
        "--unpad-paren", "--suffix=none", "--align-pointer=name",
        "--lineend=linux", "--convert-tabs",
    ]
    if check:
        cmd.extend(["--dry-run", "--error-on-changes"])
    return cmd + files


def _run_fmt(use_all: bool, check: bool, timeout: int) -> tuple[int, list[str], Path, float]:
    if not shutil.which("astyle"):
        raise UsageError("astyle is not installed; build AStyle 3.6.19 from source")
    version = _tool_version("astyle")
    if "3.6.19" not in version:
        raise UsageError(f"AStyle 3.6.19 is required, found: {version}")

    log = _new_log("fmt")
    log.parent.mkdir(parents=True, exist_ok=True)
    log.touch(exist_ok=True)
    started = time.monotonic()
    rc = 0
    checked = 0
    changed: list[str] = []
    whitespace: list[str] = []
    for repo in _fmt_repos():
        files = _changed_sources(repo, use_all)
        if not files:
            continue
        checked += len(files)
        run_rc, _, _ = _run(
            _astyle_cmd(files, check), log, timeout, cwd=repo, append=True
        )
        rc = rc or run_rc
        for line in _read_body(log).splitlines():
            if line.startswith("Formatted "):
                name = line.split(None, 1)[1].strip()
                if name not in changed:
                    changed.append(name)
        diff_rc, _, diff_log = _run(
            ["git", "-C", str(repo), "diff", "--check"],
            log.with_name(log.stem + "-diff.log"),
            timeout,
            append=False,
        )
        if diff_rc != 0:
            rc = diff_rc
            whitespace.extend(_tail(_read_body(diff_log), 10))

    elapsed = time.monotonic() - started
    if rc == 0:
        if check:
            lines = [f"[PASS] fmt clean ({checked} files)"]
        elif changed:
            lines = [f"[PASS] fmt formatted {len(changed)} files"]
            lines.extend(f"  {name}" for name in changed)
        else:
            lines = [f"[PASS] fmt clean ({checked} files)"]
    else:
        lines = [f"[FAIL] fmt {'check' if check else 'format'} ({checked} files)"]
        lines.extend(f"  {name}" for name in changed[:20])
        if len(changed) > 20:
            lines.append(f"  ... {len(changed) - 20} more files in log")
        lines.extend(whitespace)
    return rc, lines, log, elapsed


def _git_run(repo: Path, args: list[str], log: Path,
             timeout: int = 120) -> tuple[int, str]:
    rc, _, _ = _run(
        ["git", "-C", str(repo), "--no-pager", "-c", "color.ui=false", *args],
        log,
        timeout,
        append=log.exists(),
    )
    return rc, _read_body(log)


def _git_status(repo: Path, full: bool) -> tuple[int, list[str], Path, float]:
    started = time.monotonic()
    log = _new_log("git-status")
    rc, text = _git_run(repo, ["status", "--porcelain=v2", "-b"], log)
    lines = [line for line in text.splitlines() if not line.startswith("# command:")]
    if not lines:
        lines = ["[PASS] git status clean"]
    else:
        lines = ["git status:", *lines]

    sub_log = log.with_name(log.stem + "-sub.log")
    _git_run(repo, ["submodule", "status", "--recursive"], sub_log)
    subs = [
        line for line in _read_body(sub_log).splitlines()
        if not line.startswith("# ")
    ]
    if subs:
        lines.append("submodules:")
        lines.extend(subs)
        for entry in subs:
            parts = entry.split()
            if len(parts) < 2 or entry.startswith("-"):
                continue
            path = repo / parts[1]
            if not (path / ".git").exists():
                continue
            dirty = subprocess.run(
                ["git", "-C", str(path), "status", "--porcelain"],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                timeout=10,
                check=False,
            ).stdout.splitlines()
            if dirty:
                lines.append(f"  dirty {parts[1]}:")
                lines.extend(f"    {line}" for line in dirty[:5])
                if len(dirty) > 5:
                    lines.append(f"    ... {len(dirty) - 5} more paths")
    return rc, lines, log, time.monotonic() - started


def _git_diff(repo: Path, args: argparse.Namespace) -> tuple[int, list[str], Path, float]:
    started = time.monotonic()
    log = _new_log("git-diff")
    cmd = ["diff"]
    if args.staged:
        cmd.append("--cached")
    if args.check:
        cmd.append("--check")
    elif args.patch:
        cmd.extend(["-U2"])
    else:
        cmd.append("--stat")
    if args.paths:
        cmd.extend(["--", *args.paths])
    rc, text = _git_run(repo, cmd, log)
    body = [
        line for line in text.splitlines() if not line.startswith("# ")
    ]
    if args.check:
        if rc == 0:
            lines = ["[PASS] git diff --check clean"]
        else:
            lines = ["[FAIL] git diff --check", *body]
    elif not body:
        lines = ["[PASS] git diff clean"]
    elif args.patch:
        lines = body
        if len(lines) > PATCH_MAX_LINES and not args.full:
            lines = lines[:PATCH_MAX_LINES] + [
                f"... [patch truncated at {PATCH_MAX_LINES} lines; see log]"
            ]
    else:
        lines = body
    return rc, lines, log, time.monotonic() - started


def _git_log(repo: Path, count: int) -> tuple[int, list[str], Path, float]:
    started = time.monotonic()
    log = _new_log("git-log")
    rc, text = _git_run(
        repo, ["log", "--oneline", "--no-decorate", f"-n{count}"], log
    )
    lines = [
        line for line in text.splitlines() if not line.startswith("# ")
    ] or ["[PASS] no commits"]
    return rc, lines, log, time.monotonic() - started


def _git_show(repo: Path, rev: str, patch: bool) -> tuple[int, list[str], Path, float]:
    started = time.monotonic()
    log = _new_log("git-show")
    cmd = ["show", rev] if patch else ["show", "--stat", rev]
    rc, text = _git_run(repo, cmd, log)
    lines = [
        line for line in text.splitlines() if not line.startswith("# ")
    ]
    if patch and len(lines) > PATCH_MAX_LINES:
        lines = lines[:PATCH_MAX_LINES] + [
            f"... [patch truncated at {PATCH_MAX_LINES} lines; see log]"
        ]
    return rc, lines, log, time.monotonic() - started


def _sim_summary(rc: int, seconds: float, log: Path,
                 log_dir: Path) -> list[str]:
    text = _read_body(log)
    lines_in = text.splitlines()
    passed: list[str] = []
    failed: list[str] = []
    detail: dict[str, list[str]] = {}
    current: str | None = None
    for line in lines_in:
        match = re.match(r"^(\S+\.json)\s+(PASS|FAIL)\s*$", line)
        if match:
            current = match.group(1)
            if match.group(2) == "PASS":
                passed.append(current)
            else:
                failed.append(current)
                detail.setdefault(current, [])
            continue
        if current and line.strip().startswith("- "):
            detail.setdefault(current, []).append(line.strip())
    if rc == 0 and not failed:
        return [
            f"[PASS] sim {len(passed)}/{len(passed)} scenarios "
            f"({seconds:.1f}s)",
            f"logs: {_rel(log_dir)}",
        ]
    lines = [
        f"[FAIL] sim rc={rc} ({len(passed)} passed, {len(failed)} failed, "
        f"{seconds:.1f}s)"
    ]
    for name in failed[:5]:
        lines.append(f"FAILED {name}")
        lines.extend(f"  {item}" for item in detail.get(name, [])[:8])
    if not failed:
        lines.extend(_tail(text, 20))
    return lines + [f"logs: {_rel(log_dir)}"]



def _ui_summary(rc: int, seconds: float, report: Path,
                shots: Path, log: Path) -> list[str]:
    if report.is_file():
        try:
            data = json.loads(report.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            data = {}
        checked = int(data.get("checked", 0))
        failures = data.get("failures") or []
        if rc == 0 and not failures:
            return [
                f"[PASS] ui {checked} states (visual={data.get('visual_checked', 0)}, "
                f"{seconds:.1f}s)",
                f"shots: {_rel(shots)}",
            ]
        lines = [f"[FAIL] ui {checked} states, {len(failures)} failures "
                 f"({seconds:.1f}s)"]
        lines.extend(f"  - {failure}" for failure in failures[:10])
        if len(failures) > 10:
            lines.append(f"  ... {len(failures) - 10} more failures in report")
        return lines + [f"shots: {_rel(shots)}"]
    return [
        f"[FAIL] ui rc={rc} ({seconds:.1f}s); no report produced",
        *_tail(_read_body(log), 20),
        f"shots: {_rel(shots)}",
    ]


def _cmd_list(_: argparse.Namespace) -> int:
    print(f"{'alias':24} {'sanitizer':34} tags")
    for suite in SUITES.values():
        print(
            f"{suite.alias:24} {suite.sanitizer_var or '-':34} "
            f"{','.join(suite.tags)}"
        )
    return 0


def _cmd_build(args: argparse.Namespace) -> int:
    started = time.monotonic()
    if args.target == "idf":
        rc, lines, log, _ = _build_idf(args.clean, args.timeout)
    elif args.target == "sim":
        rc, lines, log, _ = _build_sim(args.clean, args.timeout)
    else:
        suite = SUITES.get(args.target)
        if suite is None:
            raise UsageError(
                f"unknown suite {args.target!r}; run `mt.py list`"
            )
        rc, lines, log, _ = _build_host(
            suite, args.profile, args.clean, args.timeout, args.full
        )
    return _finish(lines, log, "build", args.target, rc, started, args.full)


def _cmd_test(args: argparse.Namespace) -> int:
    suite = SUITES.get(args.suite)
    if suite is None:
        raise UsageError(f"unknown suite {args.suite!r}; run `mt.py list`")
    started = time.monotonic()
    rc, lines, log, _ = _test_host(
        suite, args.profile, args.clean, args.timeout,
        args.filter, args.repeat, args.full,
    )
    return _finish(lines, log, "test", args.suite, rc, started, args.full)


def _cmd_py(args: argparse.Namespace) -> int:
    requested = args.groups
    needs_contract = (not requested or "all" in requested
                      or bool({"configuration", "contracts"} & set(requested)))
    if needs_contract and not (ROOT / "contracts/device_link/.git").exists():
        raise UsageError(
            "contracts/device_link is not initialized; run "
            "`git submodule update --init --recursive`"
        )
    started = time.monotonic()
    log = _new_log("py")
    cmd = [sys.executable, str(ROOT / "tests/run_python_tests.py"), *requested]
    rc, seconds, _ = _run(cmd, log, args.timeout)
    return _finish(
        _py_summary(requested, rc, seconds, log),
        log, "py", " ".join(requested) or "all", rc, started, args.full,
    )


def _cmd_git(args: argparse.Namespace) -> int:
    repo = (ROOT / args.repo).resolve() if args.repo else ROOT
    started = time.monotonic()
    if args.git_command == "status":
        rc, lines, log, _ = _git_status(repo, args.full)
    elif args.git_command == "diff":
        rc, lines, log, _ = _git_diff(repo, args)
    elif args.git_command == "log":
        rc, lines, log, _ = _git_log(repo, args.count)
    else:
        rc, lines, log, _ = _git_show(repo, args.rev, args.patch)
    return _finish(lines, log, f"git {args.git_command}", "", rc, started, args.full)


def _cmd_fmt(args: argparse.Namespace) -> int:
    started = time.monotonic()
    rc, lines, log, _ = _run_fmt(args.all, args.check, args.timeout)
    return _finish(lines, log, "fmt", "", rc, started, args.full)


def _cmd_sim(args: argparse.Namespace) -> int:
    if not os.environ.get("IDF_PATH"):
        raise UsageError("IDF_PATH is unset; source the ESP-IDF export script first")
    if not (ROOT / "build/config/sdkconfig.h").is_file():
        raise UsageError(
            "build/config/sdkconfig.h is missing; run `idf.py build` once"
        )
    started = time.monotonic()
    _prune_run_dirs("*-sim", 10)
    log = _new_log("sim")
    log_dir = LOG_ROOT / f"{_timestamp()}-sim"
    log_dir.mkdir(parents=True, exist_ok=True)
    cmd = [
        "sh", str(ROOT / "sim/ci/run_ci.sh"), "--log-dir", str(log_dir),
    ]
    for scenario in args.scenario:
        cmd.extend(["--scenario", scenario])
    rc, seconds, _ = _run(cmd, log, args.timeout)
    return _finish(
        _sim_summary(rc, seconds, log, log_dir),
        log, "sim", ",".join(args.scenario) or "all", rc, started, args.full,
    )


def _cmd_ui(args: argparse.Namespace) -> int:
    if importlib.util.find_spec("PIL") is None:
        raise UsageError(
            "Pillow is missing; install requirements-weather-assets.txt"
        )
    started = time.monotonic()
    _prune_run_dirs("*-ui-shots", 5)
    if args.no_build and not (ROOT / "build/sim/microtech_sim").is_file():
        raise UsageError(
            "build/sim/microtech_sim is missing; run `mt.py build sim`"
        )
    if not args.no_build:
        rc, lines, build_log, _ = _build_sim(False, args.timeout)
        if rc != 0:
            return _finish(
                lines, build_log, "ui build", "sim", rc, started, args.full
            )
    shots = LOG_ROOT / f"{_timestamp()}-ui-shots"
    shots.mkdir(parents=True, exist_ok=True)
    report = shots / "report.json"
    log = _new_log("ui")
    cmd = [
        sys.executable, str(ROOT / "sim/tools/review_pages.py"),
        "--port", "0", "--report", str(report),
        "--shots", str(shots),
    ]
    if args.pages:
        cmd.extend(["--pages", args.pages])
    if args.update:
        cmd.append("--update")
    else:
        cmd.append("--check")
    rc, seconds, _ = _run(cmd, log, args.timeout)
    return _finish(
        _ui_summary(rc, seconds, report, shots, log),
        log, "ui", args.pages or "all", rc, started, args.full,
    )


def _cmd_log(args: argparse.Namespace) -> int:
    logs = sorted(LOG_ROOT.glob("*.log"), key=_mtime)
    if not logs:
        print("[ENV] no agent logs yet")
        return 2
    path = logs[-1]
    if args.log:
        path = Path(args.log)
        if not path.is_file():
            print(f"[ENV] log not found: {path}")
            return 2
    text = _read_body(path)
    lines = text.splitlines()
    if args.grep:
        try:
            pattern = re.compile(args.grep)
        except re.error as error:
            print(f"[ENV] invalid --grep expression: {error}")
            return 2
        lines = [line for line in lines if pattern.search(line)]
    if args.tail:
        lines = lines[-args.tail:]
    print(f"log: {_rel(path)}")
    print("\n".join(_cap_lines(lines, max_lines=200, max_bytes=16384)))
    return 0


def _cmd_stats(args: argparse.Namespace) -> int:
    if not STATS_PATH.is_file():
        print("[ENV] no stats yet")
        return 2
    entries = []
    for line in STATS_PATH.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        try:
            entries.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    if args.last:
        entries = entries[-args.last:]
    raw = sum(int(entry.get("raw_bytes", 0)) for entry in entries)
    emitted = sum(int(entry.get("emitted_bytes", 0)) for entry in entries)
    ratio = (1 - emitted / raw) * 100 if raw else 0.0
    print(
        f"runs={len(entries)} raw={raw} B emitted={emitted} B "
        f"reduction={ratio:.1f}%"
    )
    for entry in entries[-10:]:
        print(
            f"  {entry.get('timestamp')} {entry.get('command')} "
            f"{entry.get('suite')} rc={entry.get('rc')} "
            f"{entry.get('raw_bytes')}->{entry.get('emitted_bytes')} B"
        )
    return 0


def _add_common(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--full", action="store_true",
                        help="print the complete log instead of the summary")
    parser.add_argument("--timeout", type=int, default=600,
                        help="seconds before the whole command is terminated")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="mt.py",
        description="Token-efficient wrapper around repository build/test tools.",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("list", help="list host suites").set_defaults(func=_cmd_list)

    build = sub.add_parser("build", help="build a host suite, idf, or sim")
    build.add_argument("target")
    build.add_argument("--profile", default="none",
                       choices=("none", "address", "thread"))
    build.add_argument("--clean", action="store_true")
    _add_common(build)
    build.set_defaults(func=_cmd_build)

    test = sub.add_parser("test", help="build and run a host suite")
    test.add_argument("suite")
    test.add_argument("--profile", default="none",
                      choices=("none", "address", "thread"))
    test.add_argument("--clean", action="store_true")
    test.add_argument("--filter", default=None,
                      help="ctest -R regular expression")
    test.add_argument("--repeat", default=None,
                      help="ctest --repeat argument, e.g. until-fail:3")
    _add_common(test)
    test.set_defaults(func=_cmd_test, timeout=300)

    py = sub.add_parser("py", help="run Python test groups")
    py.add_argument("groups", nargs="*")
    _add_common(py)
    py.set_defaults(func=_cmd_py, timeout=300)

    fmt = sub.add_parser("fmt", help="format or check changed C sources")
    fmt.add_argument("--all", action="store_true")
    fmt.add_argument("--check", action="store_true")
    _add_common(fmt)
    fmt.set_defaults(func=_cmd_fmt, timeout=120)

    sim = sub.add_parser("sim", help="run simulator CI scenarios")
    sim.add_argument("--scenario", action="append", default=[])
    _add_common(sim)
    sim.set_defaults(func=_cmd_sim, timeout=1800)

    ui = sub.add_parser("ui", help="run page review")
    ui.add_argument("--pages", default=None)
    ui.add_argument("--update", action="store_true")
    ui.add_argument("--no-build", action="store_true")
    _add_common(ui)
    ui.set_defaults(func=_cmd_ui, timeout=1200)

    git = sub.add_parser("git", help="compact git operations")
    git.add_argument("--repo", default=None)
    git.add_argument("--full", action="store_true")
    git_sub = git.add_subparsers(dest="git_command", required=True)
    git_sub.add_parser("status").set_defaults(func=_cmd_git)
    diff = git_sub.add_parser("diff")
    diff.add_argument("--stat", action="store_true", help="stat output (default)")
    diff.add_argument("--patch", action="store_true")
    diff.add_argument("--staged", action="store_true")
    diff.add_argument("--check", action="store_true")
    diff.add_argument("paths", nargs="*")
    diff.set_defaults(func=_cmd_git)
    log = git_sub.add_parser("log")
    log.add_argument("-n", "--count", type=int, default=10)
    log.set_defaults(func=_cmd_git)
    show = git_sub.add_parser("show")
    show.add_argument("rev", nargs="?", default="HEAD")
    show.add_argument("--stat", action="store_true", help="stat output (default)")
    show.add_argument("--patch", action="store_true")
    show.set_defaults(func=_cmd_git)

    logs = sub.add_parser("log", help="read the latest or a named log")
    logs.add_argument("log", nargs="?")
    logs.add_argument("--last", action="store_true",
                      help="read the most recent log (default when no path)")
    logs.add_argument("--grep", default=None)
    logs.add_argument("--tail", type=int, default=40)
    logs.set_defaults(func=_cmd_log)

    stats = sub.add_parser("stats", help="show wrapper output statistics")
    stats.add_argument("--last", type=int, default=0)
    stats.set_defaults(func=_cmd_stats)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        return int(args.func(args))
    except UsageError as error:
        print(f"[ENV] {error}")
        return 2
    except KeyboardInterrupt:
        print("[ENV] interrupted")
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
