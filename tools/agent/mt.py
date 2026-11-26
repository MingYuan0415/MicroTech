#!/usr/bin/env python3
"""Token-efficient command wrapper for repository agent workflows.

The wrapper is a presentation layer only: the wrapped command decides pass or
fail, and its complete output is kept under `.cache/agent/logs/`. The summary
printed to the agent is bounded, while the log path always remains available.

Usage:
    python3 tools/agent/mt.py list
    python3 tools/agent/mt.py build apps
    python3 tools/agent/mt.py build sim
    python3 tools/agent/mt.py test apps
    python3 tools/agent/mt.py py configuration resources docs
    python3 tools/agent/mt.py fmt --check
    python3 tools/agent/mt.py git status
    python3 tools/agent/mt.py sim --scenario agent_contract
    python3 tools/agent/mt.py ui --pages settings/wifi
    python3 tools/agent/mt.py doctor --json
    python3 tools/agent/mt.py device status
"""

from __future__ import annotations

import argparse
import codecs
import contextlib
import fcntl
import hashlib
import importlib.util
import json
import os
import re
import select
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

FAILURE_WARN_REPEAT = 2
FAILURE_LABEL_MAX = 160
FAILURE_SUBJECT_MAX = 240
FAILURE_HASH_LEN = 16
STATS_SCAN_LIMIT = 5000
FAILURE_GROUP_LIMIT = 20

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
ERROR_CATEGORY_PATTERNS = (
    ("toolchain", re.compile(
        r"toolchain.*not found|command not found|no such file or directory.*(?:gcc|g\+\+|xtensa|riscv)",
        re.IGNORECASE)),
    ("cmake", re.compile(r"CMake Error", re.IGNORECASE)),
    ("link", re.compile(
        r"undefined reference to|ld: cannot find|collect2: error|ld returned",
        re.IGNORECASE)),
    ("python", re.compile(
        r"Traceback \(most recent call last\)|ModuleNotFoundError|ImportError",
        re.IGNORECASE)),
    ("partition", re.compile(
        r"partition table|partition.*not found|partition.*too small",
        re.IGNORECASE)),
    ("ninja", re.compile(r"ninja: error|ninja: build stopped", re.IGNORECASE)),
    ("compile", re.compile(r"(?:^|\s)(?:fatal error|error):", re.IGNORECASE)),
)


@dataclass(frozen=True)
class Suite:
    alias: str
    source: Path
    sanitizer_var: str | None
    tags: tuple[str, ...] = ()


@dataclass(frozen=True)
class SimBuild:
    name: str
    directory: str
    product: str
    music_backend: str


SIM_BUILDS = (
    SimBuild("s3-fake", "build/sim-gate/s3-fake", "s3_reference", "fake"),
    SimBuild("s3-real", "build/sim-gate/s3-real", "s3_reference", "real"),
    SimBuild("p4-fake", "build/sim-gate/p4-fake", "p4_reference", "fake"),
    SimBuild("minimal-fake", "build/sim-gate/minimal-fake", "minimal", "fake"),
)


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


def _load_stats(limit: int = 0) -> list[dict[str, object]]:
    if not STATS_PATH.is_file():
        return []
    entries: list[dict[str, object]] = []
    try:
        with STATS_PATH.open(encoding="utf-8", errors="replace") as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                try:
                    entry = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if isinstance(entry, dict):
                    entries.append(entry)
    except OSError:
        return []
    if limit > 0:
        entries = entries[-limit:]
    return entries


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
        del lines[0]
        while lines and lines[0].startswith("# "):
            del lines[0]
        if lines and lines[0].strip() == "":
            del lines[0]
    while lines and lines[0].strip() == "":
        del lines[0]
    return "\n".join(lines)


def _read_body(path: Path) -> str:
    return _log_body(_read_log(path))


def _failure_subject(lines: list[str]) -> str:
    cleaned = [_strip_ansi(line).strip() for line in lines if line.strip()]
    for line in cleaned:
        if line.startswith("- "):
            return line[2:].strip()
    for line in cleaned:
        if line.startswith((
            "FAILED ", "FAIL:", "ERROR:", "AssertionError",
        )):
            return line
    for line in cleaned:
        if line.startswith("Traceback (most recent call last)"):
            return line
    for line in cleaned:
        if ERROR_RE.search(line):
            return line
    for line in cleaned:
        if line.startswith(("[FAIL]", "[TIMEOUT]")):
            return line
    return cleaned[0] if cleaned else ""


def _normalize_failure(text: str, *, keep_numbers: bool) -> str:
    text = _strip_ansi(text)
    text = re.sub(r"(?:[A-Za-z]:)?(?:/[^\s:]+)+/", "", text)
    text = re.sub(r"0x[0-9a-fA-F]+", "0xADDR", text)
    if not keep_numbers:
        text = re.sub(r"\b\d+(?:\.\d+)?(?:ms|s)?\b", "#", text)
    text = re.sub(r"\s+", " ", text).strip()
    limit = FAILURE_LABEL_MAX if keep_numbers else FAILURE_SUBJECT_MAX
    return text[:limit]


def _failure_signature(lines: list[str]) -> str | None:
    subject = _failure_subject(lines)
    if not subject:
        return None
    normalized = _normalize_failure(subject, keep_numbers=False)
    if not normalized:
        return None
    return hashlib.sha256(normalized.encode("utf-8")).hexdigest()[
        :FAILURE_HASH_LEN
    ]


def _failure_streak(
    command: str, suite: str, signature: str | None
) -> tuple[int, dict[str, object] | None]:
    if not signature:
        return 1, None
    count = 0
    previous: dict[str, object] | None = None
    for entry in reversed(_load_stats(STATS_SCAN_LIMIT)):
        if (str(entry.get("command", "")) != command or
                str(entry.get("suite", "")) != suite):
            continue
        if int(entry.get("rc", 0) or 0) == 0:
            break
        if str(entry.get("failure_signature") or "") != signature:
            break
        if previous is None:
            previous = entry
        count += 1
    return count + 1, previous


def _loop_hint(
    command: str, suite: str, count: int, previous: dict[str, object] | None
) -> str | None:
    if count < FAILURE_WARN_REPEAT or previous is None:
        return None
    gate = f"{command} {suite}".strip()
    timestamp = str(previous.get("timestamp", "unknown"))
    log = str(previous.get("log") or "")
    suffix = f" {log}" if log else ""
    return _cap_line(
        f"[LOOP] same failure x{count} for {gate}; previous: "
        f"{timestamp}{suffix}; step back and try a materially different "
        "hypothesis",
        300,
    )


def _finish(lines: list[str], log_path: Path, command: str, suite: str,
            rc: int, started: float, full: bool = False) -> int:
    if rc == 124:
        lines = lines + [f"[TIMEOUT] {command} {suite} exceeded its time limit"]
    failure_label: str | None = None
    failure_signature: str | None = None
    repeat_count = 0
    loop_line: str | None = None
    if rc != 0:
        failure_label = _normalize_failure(
            _failure_subject(lines), keep_numbers=True
        ) or None
        failure_signature = _failure_signature(lines)
        repeat_count, previous = _failure_streak(
            command, suite, failure_signature
        )
        loop_line = _loop_hint(command, suite, repeat_count, previous)
    if full:
        body = _read_log(log_path).rstrip().splitlines()
        if loop_line:
            body.insert(0, loop_line)
        lines = body + [f"log: {_rel(log_path)}"]
    else:
        if loop_line:
            if lines:
                anchor = 0
                for index, line in enumerate(lines):
                    if line.startswith("[FAIL]"):
                        anchor = index
                lines = [*lines[:anchor + 1], loop_line, *lines[anchor + 1:]]
            else:
                lines = [loop_line]
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
            "failure_signature": failure_signature,
            "failure_label": failure_label,
            "repeat_count": repeat_count,
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


def _diagnostic_blocks(text: str,
                       include: re.Pattern[str] | None = None) -> list[str]:
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
        block = "\n".join(lines[start:end])
        if include is not None and not include.search(block):
            continue
        out.extend(lines[start:end])
    return out


def _classify_build_error(text: str) -> str:
    for category, pattern in ERROR_CATEGORY_PATTERNS:
        if pattern.search(text):
            return category
    return "unknown"


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
    category = _classify_build_error(text)
    lines = [f"[FAIL] build {alias} category={category} rc={rc} ({seconds:.1f}s)"]
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


def _require_sim_env() -> None:
    if not os.environ.get("IDF_PATH"):
        raise UsageError("IDF_PATH is unset; source the ESP-IDF export script first")
    if not (ROOT / "build/config/sdkconfig.h").is_file():
        raise UsageError(
            "build/config/sdkconfig.h is missing; run `idf.py build` once"
        )


def _device_product() -> str:
    profile = ROOT / "build" / "mt_product_profile.h"
    try:
        with profile.open(encoding="utf-8", errors="replace") as handle:
            for line in handle:
                prefix = '#define MT_PRODUCT_NAME "'
                if line.startswith(prefix):
                    return line[len(prefix):].split('"', 1)[0]
    except OSError:
        pass
    return ""


def _build_sim_configuration(spec: SimBuild | None, clean: bool, timeout: int,
                             log: Path, append: bool) -> tuple[int, float]:
    build = ROOT / (spec.directory if spec is not None else "build/sim")
    if clean:
        shutil.rmtree(build, ignore_errors=True)
    with _exclusive_lock(build / ".sim-ci-build.lock"):
        configure = ["cmake", "-S", str(ROOT / "sim"), "-B", str(build), "-G",
                     "Ninja"]
        if spec is not None:
            configure.extend([
                f"-DSIM_PRODUCT={spec.product}",
                f"-DSIM_MUSIC_BACKEND={spec.music_backend}",
            ])
        else:
            product = _device_product()
            if product:
                configure.append(f"-DSIM_PRODUCT={product}")
        rc, seconds, _ = _run(configure, log, timeout, append=append)
        if rc != 0:
            return rc, seconds
        rc, build_seconds, _ = _run(
            ["cmake", "--build", str(build), "--", "--quiet"],
            log,
            timeout,
            append=True,
        )
    return rc, seconds + build_seconds


def _build_sim(clean: bool, timeout: int) -> tuple[int, list[str], Path, float]:
    _require_sim_env()
    log = _new_log("build-sim")
    rc, seconds = _build_sim_configuration(
        None, clean, timeout, log, append=False
    )
    if rc == 0:
        build = ROOT / "build/sim"
        if _build_context_entry(build, build / "microtech_sim", "sim") is None:
            return 1, [
                "[FAIL] build sim context missing",
                "  the simulator built but build-context.json could not be written",
                f"  log: {_rel(log)}",
            ], log, seconds
    return rc, _build_summary("sim", rc, seconds, log), log, seconds


def _context_matches_artifact(context_path: Path, artifact: Path) -> bool:
    try:
        context = json.loads(context_path.read_text(encoding="utf-8"))
        if not isinstance(context, dict):
            return False
        recorded = Path(str(context.get("artifact", "")))
        if not recorded.is_absolute():
            recorded = ROOT / recorded
        if recorded.resolve() != artifact.resolve():
            return False
        digest = hashlib.sha256()
        with artifact.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                digest.update(chunk)
        return context.get("artifact_sha256") == digest.hexdigest()
    except (OSError, ValueError, TypeError):
        return False


def _build_context_entry(build: Path, artifact: Path, kind: str) -> dict[str, object] | None:
    helper = ROOT / "sim/tools/build_context.py"
    out = build / "build-context.json"
    with contextlib.suppress(OSError):
        out.unlink()
    try:
        result = subprocess.run(
            [sys.executable, str(helper), "--build", str(build),
             "--artifact", str(artifact), "--out", str(out), "--kind", kind],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=30,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if result.returncode != 0 or not out.is_file():
        return None
    try:
        return json.loads(out.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None


def _build_sim_gate(clean: bool, timeout: int) -> tuple[int, list[str], Path, float]:
    _require_sim_env()
    log = _new_log("build-sim-gate")
    started = time.monotonic()
    failed: SimBuild | None = None
    gate_root = ROOT / "build" / "sim-gate"
    with _exclusive_lock(ROOT / "build" / ".sim-gate.lock"):
        if clean:
            shutil.rmtree(gate_root, ignore_errors=True)
        for index, spec in enumerate(SIM_BUILDS):
            rc, _ = _build_sim_configuration(
                spec, False, timeout, log, append=index != 0
            )
            if rc != 0:
                failed = spec
                break
    seconds = time.monotonic() - started
    if failed is None:
        configs = []
        context_error = None
        for spec in SIM_BUILDS:
            build = ROOT / spec.directory
            entry = _build_context_entry(build, build / "microtech_sim", "sim")
            if entry is None:
                context_error = spec.name
                break
            configs.append(entry)
        if context_error is not None:
            return 1, [
                f"[FAIL] build sim gate context missing for {context_error}",
                "  the configuration built but did not produce build-context.json",
                f"  log: {_rel(log)}",
            ], log, seconds
        context_path = gate_root / "build-context.json"
        try:
            context_path.write_text(json.dumps({
                "schema": 1,
                "kind": "sim-gate",
                "generated_at": datetime.now().astimezone().isoformat(
                    timespec="seconds"),
                "configs": configs,
            }, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
        except OSError as error:
            return 1, [
                "[FAIL] build sim gate context write failed",
                f"  {error}",
                f"  log: {_rel(log)}",
            ], log, seconds
        lines = [
            f"[PASS] build sim gate ({len(SIM_BUILDS)} configurations, "
            f"{seconds:.1f}s)"
        ]
        lines.extend(f"  {spec.name}: {spec.directory}" for spec in SIM_BUILDS)
        lines.append(f"  context: {_rel(context_path)}")
        warnings = _warning_lines(_read_body(log))
        if warnings:
            lines.append(f"  {len(warnings)} warning(s); see log")
        return 0, lines, log, seconds
    lines = [
        f"[FAIL] build sim gate {failed.name} rc={rc} ({seconds:.1f}s)",
        f"  config: product={failed.product} music={failed.music_backend}",
    ]
    blocks = _diagnostic_blocks(_read_body(log))
    lines.extend(blocks if blocks else _tail(_read_body(log)))
    return rc, lines, log, seconds


def _build_idf(clean: bool, timeout: int,
               coredump: bool = False) -> tuple[int, list[str], Path, float]:
    if not shutil.which("idf.py") or not os.environ.get("IDF_PATH"):
        raise UsageError("idf.py/IDF_PATH is unavailable; source the export script first")
    log = _new_log("build-idf")
    build = ROOT / "build"
    # A coredump build gets its own SDKCONFIG file.  Otherwise the overlay
    # values would be saved back into the developer's local sdkconfig and a
    # later plain build would silently keep coredump enabled.
    sdkconfig = build / "sdkconfig.coredump" if coredump else ROOT / "sdkconfig"
    base = ["idf.py", "-B", str(build), "-D", f"SDKCONFIG={sdkconfig}"]
    if coredump:
        base.extend([
            "-D",
            "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.coredump.defaults",
        ])
    else:
        base.extend(["-D", "SDKCONFIG_DEFAULTS=sdkconfig.defaults"])
    if clean:
        rc, seconds, _ = _run([*base, "fullclean"], log, timeout)
        if rc != 0:
            return rc, _build_summary("idf", rc, seconds, log), log, seconds
    rc, seconds, _ = _run([*base, "build"], log, max(timeout, 1800))
    if rc == 0:
        actual_coredump = _device_coredump_enabled(build)
        if coredump and not actual_coredump:
            return 1, [
                "[FAIL] build idf coredump configuration missing",
                "  the firmware built but CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH "
                "is still disabled",
                f"  config: {_rel(build / 'sdkconfig.coredump')}",
                f"  log: {_rel(log)}",
            ], log, seconds
        entry = _build_context_entry(build, build / "microtech.bin", "idf")
        if entry is None:
            return 1, [
                "[FAIL] build idf context missing",
                "  the firmware built but build-context.json could not be written",
                f"  log: {_rel(log)}",
            ], log, seconds
        with contextlib.suppress(OSError):
            (build / "mt_build_context.json").write_text(
                json.dumps(entry, ensure_ascii=False, indent=1) + "\n",
                encoding="utf-8",
            )
        lines = _build_summary("idf", rc, seconds, log)
        if coredump:
            lines.append(f"  coredump: enabled ({_rel(build / 'sdkconfig.coredump')})")
        elif actual_coredump:
            lines.append(
                "  [WARN] coredump is still enabled by the local sdkconfig; "
                "use `build idf --coredump` or reset that local setting")
        return 0, lines, log, seconds
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
               full: bool, no_build: bool = False
               ) -> tuple[int, list[str], Path, float]:
    build = _build_dir(suite.alias, profile)
    if no_build:
        if not (build / "CTestTestfile.cmake").is_file():
            raise UsageError(
                f"build/agent/host/{suite.alias}/{profile} is not configured; "
                "run `mt.py build` or `mt.py test` without --no-build first"
            )
        build_lines: list[str] = []
        build_log = _new_log(f"test-{suite.alias}-{profile}-reuse")
        build_seconds = 0.0
    else:
        rc, build_lines, build_log, build_seconds = _build_host(
            suite, profile, clean, timeout, full
        )
        if rc != 0:
            return rc, build_lines, build_log, build_seconds
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
        or "sim/third_party/" in path
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
        snapshots = int(data.get("snapshots", 0))
        failures = list(data.get("failures") or [])
        build_context_error = data.get("build_context_error")
        if (build_context_error and
                not any(str(item).startswith("build-context:")
                        for item in failures)):
            failures.append("build-context: %s" % build_context_error)
        if rc == 0 and not failures:
            return [
                f"[PASS] ui {checked} states (snapshots={snapshots}, "
                f"{seconds:.1f}s)",
                f"shots: {_rel(shots)}",
            ]
        lines = [
            f"[FAIL] ui {checked} states, {snapshots} snapshots, "
            f"{len(failures)} failures ({seconds:.1f}s)"
        ]
        lines.extend(f"  - {failure}" for failure in failures[:10])
        if len(failures) > 10:
            lines.append(f"  ... {len(failures) - 10} more failures in report")
        return lines + [f"shots: {_rel(shots)}"]
    return [
        f"[FAIL] ui rc={rc} ({seconds:.1f}s); no report produced",
        *_tail(_read_body(log), 20),
        f"shots: {_rel(shots)}",
    ]


def _device_build_meta() -> dict[str, object]:
    meta: dict[str, object] = {
        "build_dir": str(ROOT / "build"),
        "target": "",
        "product": "",
        "app_version": "",
        "elf_sha256": "",
        "app_bin": "",
        "app_elf": "",
    }
    build_dir = ROOT / "build"
    description = build_dir / "project_description.json"
    if description.is_file():
        try:
            data = json.loads(description.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            data = {}
        if isinstance(data, dict):
            raw_build_dir = data.get("build_dir")
            if isinstance(raw_build_dir, str) and raw_build_dir:
                candidate = Path(raw_build_dir).expanduser()
                build_dir = (candidate if candidate.is_absolute()
                             else ROOT / candidate)
            meta["build_dir"] = str(build_dir)
            meta["target"] = data.get("target", "")
            meta["app_version"] = data.get("project_version", "")
            for key in ("app_bin", "app_elf"):
                name = data.get(key)
                if isinstance(name, str) and name:
                    candidate = Path(name).expanduser()
                    meta[key] = str(candidate if candidate.is_absolute()
                                    else build_dir / candidate)
    profile = build_dir / "mt_product_profile.h"
    if profile.is_file():
        for line in profile.read_text(encoding="utf-8",
                                      errors="replace").splitlines():
            prefix = '#define MT_PRODUCT_NAME "'
            if line.startswith(prefix):
                meta["product"] = line[len(prefix):].split('"', 1)[0]
                break
    if not meta["target"]:
        sdkconfig = build_dir / "config/sdkconfig.h"
        if not sdkconfig.is_file():
            sdkconfig = build_dir / "gen_inc/sdkconfig.h"
        if sdkconfig.is_file():
            match = re.search(r'^#define CONFIG_IDF_TARGET "([^"]+)"',
                              sdkconfig.read_text(encoding="utf-8",
                                                  errors="replace"),
                              re.MULTILINE)
            if match:
                meta["target"] = match.group(1)
    app_bin = str(meta.get("app_bin", ""))
    if app_bin and Path(app_bin).is_file() and not meta["elf_sha256"]:
        try:
            result = subprocess.run(
                [sys.executable, "-m", "esptool", "image-info", app_bin],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=20,
                check=False,
            )
            text = result.stdout or ""
            match = re.search(r"^App version:\s*(.+)$", text, re.MULTILINE)
            if match:
                meta["app_version"] = match.group(1).strip()
            match = re.search(r"^ELF file SHA256:\s*([0-9a-fA-F]+)$",
                              text, re.MULTILINE)
            if match:
                meta["elf_sha256"] = match.group(1).lower()
        except (OSError, subprocess.SubprocessError):
            pass
    return meta


def _device_serial_ports() -> list[dict[str, str]]:
    try:
        from serial.tools import list_ports  # type: ignore
    except ImportError:
        dev = Path("/dev")
        return [{"device": str(path), "description": "serial"}
                for pattern in ("ttyUSB*", "ttyACM*")
                for path in sorted(dev.glob(pattern))]
    return [
        {"device": port.device, "description": port.description or ""}
        for port in list_ports.comports()
    ]


def _device_preferred_ports(ports: list[dict[str, str]]) -> list[dict[str, str]]:
    """Prefer USB-backed ports over anonymous legacy ttyS entries."""
    return [
        port for port in ports
        if port["device"].startswith(("/dev/ttyACM", "/dev/ttyUSB", "/dev/cu."))
        or port["description"].strip().lower() not in ("", "n/a")
    ]


def _device_port(explicit: str | None, required: bool = False) -> str | None:
    if explicit:
        return explicit
    ports = _device_serial_ports()
    preferred = _device_preferred_ports(ports)
    if len(preferred) == 1:
        return preferred[0]["device"]
    if len(preferred) > 1 and required:
        listing = ", ".join(f"{item['device']} ({item['description']})"
                            for item in preferred)
        raise UsageError(f"multiple serial ports found: {listing}; pass --port")
    if len(ports) == 1:
        return ports[0]["device"]
    if not ports:
        if required:
            raise UsageError("no serial port found; connect a board or pass --port")
        return None
    if not required:
        return None
    listing = ", ".join(f"{item['device']} ({item['description']})"
                        for item in ports)
    raise UsageError(f"multiple serial ports found: {listing}; pass --port")


def _device_partition_info(build_dir: Path | None = None) -> dict[str, str]:
    table_name = "partitions_16m_single_app_res_data.csv"
    build_dir = build_dir or ROOT / "build"
    sdkconfig = build_dir / "config/sdkconfig.h"
    if not sdkconfig.is_file():
        sdkconfig = build_dir / "gen_inc/sdkconfig.h"
    if sdkconfig.is_file():
        match = re.search(
            r'^#define CONFIG_PARTITION_TABLE_CUSTOM_FILENAME "([^"]+)"',
            sdkconfig.read_text(encoding="utf-8", errors="replace"),
            re.MULTILINE,
        )
        if match:
            table_name = match.group(1)
    table = ROOT / table_name
    if not table.is_file():
        return {}
    for line in table.read_text(encoding="utf-8", errors="replace").splitlines():
        parts = [part.strip() for part in line.split(",")]
        if len(parts) >= 6 and parts[0] == "coredump":
            return {"offset": parts[3], "size": parts[4]}
    return {}


def _device_coredump_enabled(build_dir: Path | None = None) -> bool:
    build_dir = build_dir or ROOT / "build"
    sdkconfig = build_dir / "config/sdkconfig.h"
    if not sdkconfig.is_file():
        sdkconfig = build_dir / "gen_inc/sdkconfig.h"
    if not sdkconfig.is_file():
        return False
    return bool(re.search(
        r"^#define CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH 1$",
        sdkconfig.read_text(encoding="utf-8", errors="replace"),
        re.MULTILINE,
    ))


def _device_monitor_command(port: str, reset: bool, elf: Path,
                            timestamps: bool = True) -> list[str]:
    meta = _device_build_meta()
    command = [
        sys.executable, "-m", "esp_idf_monitor", str(elf),
        "--port", port,
        "--baud", "115200",
        "--target", str(meta.get("target") or "esp32s3"),
        "--decode-panic", "backtrace",
        "--decode-coredumps", "info",
    ]
    if timestamps:
        command.append("--timestamps")
    if not reset:
        command.append("--no-reset")
    return command


def _run_with_input(cmd: list[str], log_path: Path, timeout: float | None,
                    script: str | None) -> tuple[int, float, int]:
    LOG_ROOT.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    with log_path.open("w", encoding="utf-8", errors="replace") as fh:
        with contextlib.suppress(OSError):
            os.chmod(log_path, 0o600)
        fh.write(f"# command: {' '.join(cmd)}\n")
        fh.write(f"# cwd: {ROOT}\n\n")
        fh.flush()
        proc = subprocess.Popen(
            cmd,
            cwd=str(ROOT),
            stdout=fh,
            stderr=subprocess.STDOUT,
            stdin=subprocess.PIPE if script is not None else subprocess.DEVNULL,
            text=True,
            errors="replace",
            env=_base_env(),
            start_new_session=True,
        )
        try:
            # communicate() owns the stdin write and wait loop, so a child that
            # stops reading cannot block the wrapper past its timeout.
            proc.communicate(input=script, timeout=timeout)
            rc = proc.returncode if proc.returncode is not None else 0
        except subprocess.TimeoutExpired:
            rc = 124
            _terminate(proc)
            fh.write(f"\n# timeout after {timeout}s\n")
        except KeyboardInterrupt:
            _terminate(proc)
            raise
        finally:
            if proc.stdin is not None:
                with contextlib.suppress(OSError):
                    proc.stdin.close()
    return rc, time.monotonic() - started, log_path.stat().st_size


def _run_streaming(cmd: list[str], log_path: Path,
                   timeout: float | None = None) -> tuple[int, float, int]:
    """Run a monitor in the foreground while mirroring output to its log."""
    LOG_ROOT.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    with log_path.open("w", encoding="utf-8", errors="replace") as fh:
        with contextlib.suppress(OSError):
            os.chmod(log_path, 0o600)
        fh.write(f"# command: {' '.join(cmd)}\n")
        fh.write(f"# cwd: {ROOT}\n\n")
        fh.flush()
        proc = subprocess.Popen(
            cmd,
            cwd=str(ROOT),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
            text=False,
            env=_base_env(),
            start_new_session=True,
        )
        rc: int | None = None
        decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")

        def emit(text: str) -> None:
            if not text:
                return
            fh.write(text)
            fh.flush()
            sys.stdout.write(text)
            sys.stdout.flush()

        try:
            assert proc.stdout is not None
            while True:
                if timeout is not None:
                    remaining = timeout - (time.monotonic() - started)
                    if remaining <= 0:
                        rc = 124
                        _terminate(proc)
                        fh.write(f"\n# timeout after {timeout}s\n")
                        break
                    wait = min(0.5, remaining)
                else:
                    wait = 0.5
                ready, _, _ = select.select([proc.stdout], [], [], wait)
                if not ready:
                    continue
                # Reading a chunk instead of readline() keeps a half-written
                # serial line from blocking past the timeout: select() only
                # guarantees that some bytes are available, not a full line.
                chunk = os.read(proc.stdout.fileno(), 4096)
                if not chunk:
                    # A multi-byte character split across the last read is
                    # still buffered in the incremental decoder. Flush it so
                    # the log contains exactly the bytes the monitor emitted.
                    emit(decoder.decode(b"", final=True))
                    break
                emit(decoder.decode(chunk))
            if rc is None:
                rc = proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            rc = 124
            _terminate(proc)
            fh.write(f"\n# timeout after {timeout}s\n")
        except KeyboardInterrupt:
            _terminate(proc)
            raise
        finally:
            if proc.stdout is not None:
                with contextlib.suppress(OSError):
                    proc.stdout.close()
    return rc if rc is not None else 124, time.monotonic() - started, \
        log_path.stat().st_size


def _device_status_data(args: argparse.Namespace) -> dict[str, object]:
    meta = _device_build_meta()
    ports = _device_serial_ports()
    port = _device_port(args.port, required=bool(args.probe))
    data: dict[str, object] = {
        "port": port,
        "ports": ports,
        "build_dir": meta.get("build_dir", ""),
        "target": meta.get("target", ""),
        "product": meta.get("product", ""),
        "app": {
            "version": meta.get("app_version", ""),
            "elf_sha256": meta.get("elf_sha256", ""),
            "bin": meta.get("app_bin", ""),
            "elf": meta.get("app_elf", ""),
        },
        "coredump": {
            "enabled": _device_coredump_enabled(
                Path(str(meta.get("build_dir") or ROOT / "build"))),
            **_device_partition_info(
                Path(str(meta.get("build_dir") or ROOT / "build"))),
        },
    }
    app = data["app"]
    data["build_ready"] = bool(
        data["target"] and app["version"] and app["elf_sha256"] and
        app["bin"] and Path(str(app["bin"])).is_file() and
        app["elf"] and Path(str(app["elf"])).is_file()
    )
    if args.probe:
        if not port:
            raise UsageError("--probe needs a serial port; pass --port")
        log = _new_log("device-probe")
        rc, _, _ = _run(
            [sys.executable, "-m", "esptool", "--port", port, "read-mac"],
            log, 30,
        )
        probe: dict[str, str] = {}
        if rc == 0:
            match = re.search(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}",
                              _read_body(log))
            probe["mac"] = match.group(0) if match else ""
        else:
            probe["error"] = "MAC probe failed"
        chip_log = _new_log("device-probe-chip")
        chip_rc, _, _ = _run(
            [sys.executable, "-m", "esptool", "--port", port, "chip-id"],
            chip_log, 30,
        )
        if chip_rc == 0:
            chip_text = _read_body(chip_log)
            match = re.search(r"Chip type:\s*([^\n(]+)", chip_text,
                              re.IGNORECASE)
            if match:
                probe["chip"] = match.group(1).strip()
            else:
                match = re.search(r"Chip ID:\s*([^\s]+)", chip_text,
                                  re.IGNORECASE)
                probe["chip"] = match.group(1) if match else ""
        else:
            probe["chip_error"] = "chip probe failed"
        data["probe"] = probe
    return data


def _device_probe_ok(data: dict[str, object]) -> bool:
    probe = data.get("probe")
    if not isinstance(probe, dict):
        return False
    mac = probe.get("mac")
    chip = probe.get("chip")
    if not mac or not chip:
        return False
    target = str(data.get("target") or "")
    if not target:
        return False
    normalize = lambda value: re.sub(r"[^a-z0-9]", "", value.lower())
    return normalize(target) == normalize(str(chip))


def _cmd_device_status(args: argparse.Namespace) -> int:
    data = _device_status_data(args)
    probe_failed = bool(args.probe) and not _device_probe_ok(data)
    build_failed = not bool(data.get("build_ready"))
    failed = probe_failed or build_failed
    if args.json:
        print(json.dumps(data, ensure_ascii=False, indent=1))
        return 1 if failed else 0
    app = data["app"]
    coredump = data["coredump"]
    print("[FAIL] device status" if failed else "[PASS] device status")
    print(f"  port: {data['port'] or '-'}")
    print(f"  target: {data['target']}  product: {data['product']}")
    print(f"  app: version={app['version']} elf={app['elf_sha256'][:16]}")
    print(f"  bin: {app['bin']}")
    print(f"  coredump: enabled={coredump['enabled']} "
          f"offset={coredump.get('offset', '-')} size={coredump.get('size', '-')}")
    if "probe" in data:
        probe = data["probe"]
        print(f"  probe: {probe}")
    if build_failed:
        print("  local build: incomplete (build and flash identity cannot be read)")
    return 1 if failed else 0


def _monitor_connection_failed(body: str) -> bool:
    for line in body.splitlines():
        stripped = line.strip()
        if (stripped.startswith("Could not configure port") or
                stripped.startswith("Failed to open port")):
            return True
        if (stripped.startswith("--- ERROR: Connection to") and
                " failed" in stripped):
            return True
    return False


def _normalize_monitor_rc(rc: int, log: Path) -> int:
    """Map esp-idf-monitor's non-fatal exits to wrapper statuses."""
    body = _read_body(log)
    if _monitor_connection_failed(body):
        return 1
    if rc == 110 and "Expect pattern" in body and "timed out" in body:
        return 124
    return rc


def _device_monitor_lines(rc: int, log: Path, expect: str | None,
                          require_output: bool = False) -> list[str]:
    body = _read_body(log)
    output_lines = [
        line for line in body.splitlines()
        if line.strip() and
        not line.lstrip().startswith("---") and
        "Command:" not in line and
        "Expect pattern" not in line
    ]
    match_lines = [_strip_monitor_timestamp(line) for line in output_lines]
    message_lines = [_strip_monitor_message_prefix(line) for line in match_lines]
    if _monitor_connection_failed(body):
        lines = ["[FAIL] device monitor: serial connection failed"]
    elif rc == 0 and expect is None:
        if output_lines or not require_output:
            lines = ["[PASS] device logs"]
        else:
            lines = ["[FAIL] device logs: no serial output captured"]
    elif rc == 0 and expect is not None:
        candidates = [*match_lines, *message_lines]
        if any(re.search(expect, line) for line in candidates):
            lines = [f"[PASS] device monitor matched {expect!r}"]
        else:
            lines = [f"[FAIL] device monitor did not find {expect!r} in output"]
    elif rc == 124:
        suffix = f" after {expect!r}" if expect is not None else ""
        lines = [f"[TIMEOUT] device monitor{suffix}"]
    elif rc == 130:
        lines = ["[INTERRUPTED] device monitor stopped by user"]
    else:
        lines = [f"[FAIL] device monitor rc={rc}"]
    lines.extend(f"  {line}" for line in _tail(body, 12))
    return lines


_NORMAL_BOOT_REASONS = {"POWERON", "USB", "SW", "EXT", "JTAG", "DEEPSLEEP"}
_MONITOR_TIMESTAMP_RE = re.compile(
    r"^\d{4}-\d{2}-\d{2}[ T]\d{2}:\d{2}:\d{2}(?:[.,]\d+)?\s+"
)
_MONITOR_MESSAGE_RE = re.compile(r"^[A-Z] \(\d+\) [^:]+: ")


def _strip_monitor_timestamp(line: str) -> str:
    return _MONITOR_TIMESTAMP_RE.sub("", line, count=1)


def _strip_monitor_message_prefix(line: str) -> str:
    return _MONITOR_MESSAGE_RE.sub("", line, count=1)


def _boot_banner(log: Path) -> dict[str, str] | None:
    matches = list(re.finditer(
        r"boot id=([0-9a-fA-F]+)\s+reason=(\S+)\s+version=(\S+)\s+"
        r"elf=([0-9a-fA-F]+)\s+partition=(\S+)",
        _read_body(log),
    ))
    if not matches:
        return None
    match = matches[-1]
    return {
        "id": match.group(1).lower(),
        "reason": match.group(2),
        "version": match.group(3),
        "elf": match.group(4).lower(),
        "partition": match.group(5),
    }


def _boot_banner_check(log: Path, meta: dict[str, object]) -> tuple[list[str], bool]:
    """Validate the newest boot banner against the local build identity."""
    banner = _boot_banner(log)
    if banner is None:
        return ["[FAIL] malformed boot banner"], False
    lines = [
        "  boot: id=%s reason=%s version=%s elf=%s partition=%s" %
        (banner["id"], banner["reason"], banner["version"],
         banner["elf"], banner["partition"])
    ]
    issues: list[str] = []
    if banner["reason"] not in _NORMAL_BOOT_REASONS:
        issues.append("abnormal reset reason %s" % banner["reason"])
    expected_elf = str(meta.get("elf_sha256") or "").lower()
    booted_elf = banner["elf"]
    if not expected_elf:
        issues.append("local ELF SHA256 unavailable")
    elif not (booted_elf == expected_elf or
              (booted_elf and expected_elf.startswith(booted_elf))):
        issues.append(
            "booted ELF does not match local build "
            "(expected %s, booted %s)" % (expected_elf, booted_elf))
    if not banner["partition"] or banner["partition"] == "unknown":
        issues.append("boot partition is unknown")
    expected_version = str(meta.get("app_version") or "")
    if expected_version and banner["version"] != expected_version:
        issues.append(
            "booted version %s does not match local %s" %
            (banner["version"], expected_version))
    if issues:
        lines.extend("[FAIL] %s" % issue for issue in issues)
        return lines, False
    lines.append("  elf matches local build")
    return lines, True


def _cmd_device_logs(args: argparse.Namespace) -> int:
    if args.timeout is not None and args.timeout <= 0:
        raise UsageError("--timeout must be greater than zero")
    if not args.follow and args.seconds <= 0:
        raise UsageError("--seconds must be greater than zero")
    pattern = None
    if args.grep:
        try:
            pattern = re.compile(args.grep)
        except re.error as error:
            raise UsageError(f"invalid --grep: {error}") from error
    port = _device_port(args.port, required=True)
    meta = _device_build_meta()
    elf = Path(str(meta.get("app_elf") or ROOT / "build/microtech.elf"))
    if not elf.is_file():
        raise UsageError(f"ELF not found: {_rel(elf)}; build the firmware first")
    log = _new_log("device-logs")
    command = _device_monitor_command(port, args.reset, elf)
    started = time.monotonic()
    try:
        if args.follow:
            rc, seconds, _ = _run_streaming(command, log,
                                            timeout=args.timeout)
        else:
            timeout = (args.timeout if args.timeout is not None
                       else args.seconds + 10)
            rc, seconds, _ = _run_with_input(
                command, log, timeout,
                f"sleep {args.seconds}\nexit\n",
            )
    except KeyboardInterrupt:
        rc = 130
    rc = _normalize_monitor_rc(rc, log)
    # A log capture with no device output is not evidence, even when the
    # monitor process itself exits cleanly (for example on a disconnected
    # board or an incompatible ELF).
    lines = _device_monitor_lines(rc, log, None, require_output=True)
    if rc == 0 and lines and lines[0].startswith("[FAIL]"):
        rc = 1
    if pattern is not None:
        body = _read_body(log)
        # --grep is a filter, not a second copy of the log: keep the status
        # headline and only the matching output lines.
        lines = lines[:1] + [
            f"  {line}" for line in body.splitlines() if pattern.search(line)
        ]
    return _finish(lines, log, "device logs",
                   "follow" if args.follow else "snapshot", rc, started)


def _monitor_command_expect(expect: str) -> str:
    """Remove only the outer anchors that the monitor cannot see past its
    ANSI/log prefix; the wrapper still applies the original regex to cleaned
    output lines."""
    pattern = expect
    if pattern.startswith("^"):
        pattern = pattern[1:]
    if pattern.endswith("$") and not pattern.endswith("\\$"):
        pattern = pattern[:-1]
    return pattern or ".*"


def _cmd_device_monitor(args: argparse.Namespace) -> int:
    if args.timeout <= 0:
        raise UsageError("--timeout must be greater than zero")
    port = _device_port(args.port, required=True)
    meta = _device_build_meta()
    elf = Path(str(meta.get("app_elf") or ROOT / "build/microtech.elf"))
    if not elf.is_file():
        raise UsageError(f"ELF not found: {_rel(elf)}; build the firmware first")
    try:
        re.compile(args.expect)
    except re.error as error:
        raise UsageError(f"invalid --expect expression: {error}") from error
    log = _new_log("device-monitor")
    # The monitor's own expect command sees the raw line; prepending
    # timestamps would make anchored expressions such as ^boot id= impossible.
    command = _device_monitor_command(port, args.reset, elf,
                                      timestamps=False)
    started = time.monotonic()
    rc, seconds, _ = _run_with_input(
        command, log, args.timeout + 10,
        "expect --timeout %d %s\n" %
        (args.timeout, _monitor_command_expect(args.expect)),
    )
    rc = _normalize_monitor_rc(rc, log)
    lines = _device_monitor_lines(rc, log, args.expect)
    if rc == 0 and lines and lines[0].startswith("[FAIL]"):
        rc = 1
    return _finish(lines, log, "device monitor", args.expect, rc, started)


def _cmd_device_crash(args: argparse.Namespace) -> int:
    meta = _device_build_meta()
    if not _device_coredump_enabled(Path(str(meta.get("build_dir") or
                                                ROOT / "build"))):
        print("[ENV] flash coredump is disabled; build with "
              "`mt.py build idf --coredump` or enable "
              "CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH in local sdkconfig")
        return 2
    port = _device_port(args.port, required=True)
    elf = Path(str(meta.get("app_elf") or ROOT / "build/microtech.elf"))
    if not elf.is_file():
        raise UsageError(f"ELF not found: {_rel(elf)}; build the firmware first")
    log = _new_log("device-crash")
    build_dir = str(meta.get("build_dir") or ROOT / "build")
    command = ["idf.py", "-B", build_dir, "coredump-info", "-p", port]
    if args.save_core:
        command.extend(["--save-core", args.save_core])
    started = time.monotonic()
    rc, seconds, _ = _run(command, log, max(args.timeout, 60))
    if rc == 0:
        lines = ["[PASS] coredump decoded"]
    else:
        lines = [f"[FAIL] coredump decode rc={rc}"]
    lines.extend(f"  {line}" for line in _tail(_read_body(log), 30))
    return _finish(lines, log, "device crash", port, rc, started)


def _cmd_device_flash(args: argparse.Namespace) -> int:
    if not args.yes:
        print("[ENV] device flash requires --yes; refusing to flash")
        return 2
    # Validate every flash-adjacent option before the first write. A bad
    # monitor timeout must never leave the board flashed but unverified.
    monitor_timeout = getattr(args, "monitor_timeout", 20)
    if args.monitor and monitor_timeout <= 0:
        raise UsageError("--monitor-timeout must be greater than zero")
    port = _device_port(args.port, required=True)
    log = _new_log("device-flash")
    started = time.monotonic()
    before = _device_status_data(
        argparse.Namespace(port=port, probe=False, json=True))
    if not before.get("build_ready"):
        lines = [
            "[FAIL] device flash refused: local build is incomplete",
            f"  build: {before.get('build_dir', ROOT / 'build')}",
        ]
        return _finish(lines, log, "device flash", port, 1,
                       time.monotonic() - started)
    before = _device_status_data(
        argparse.Namespace(port=port, probe=True, json=True))
    if not _device_probe_ok(before):
        lines = [
            "[FAIL] device flash refused: pre-flash identity probe failed",
            f"  probe: {before.get('probe')}",
        ]
        return _finish(lines, log, "device flash", port, 1,
                       time.monotonic() - started)
    build_dir = str(before.get("build_dir") or ROOT / "build")
    rc, seconds, _ = _run(
        ["idf.py", "-B", build_dir, "-p", port, "flash"], log, args.timeout)
    if rc != 0:
        lines = [f"[FAIL] device flash rc={rc}", *_tail(_read_body(log), 20)]
        return _finish(lines, log, "device flash", port, rc,
                       time.monotonic() - started)
    after = _device_status_data(
        argparse.Namespace(port=port, probe=True, json=True))
    before_mac = (before.get("probe") or {}).get("mac")
    after_mac = (after.get("probe") or {}).get("mac")
    details: list[str] = []
    verify_failed = False
    if (not _device_probe_ok(before) or not _device_probe_ok(after) or
            not after.get("build_ready")):
        details.append("[FAIL] post-flash identity probe failed")
        verify_failed = True
    else:
        details.append(f"  mac: {before_mac} -> {after_mac}")
        if before_mac != after_mac:
            details.append("[FAIL] device identity changed during flash")
            verify_failed = True
    if args.monitor:
        meta = _device_build_meta()
        elf = Path(str(meta.get("app_elf") or
                       ROOT / "build/microtech.elf"))
        monitor_log = log.with_name(log.stem + "-monitor.log")
        monitor_rc = _run_with_input(
            _device_monitor_command(port, True, elf, timestamps=False),
            monitor_log, monitor_timeout + 10,
            f"expect --timeout {monitor_timeout} boot id=\nexit\n")[0]
        monitor_rc = _normalize_monitor_rc(monitor_rc, monitor_log)
        if monitor_rc != 0:
            details.append(f"[FAIL] device monitor rc={monitor_rc}")
            details.extend(f"  {line}" for line in
                           _tail(_read_body(monitor_log), 12))
            verify_failed = True
        else:
            banner_lines, banner_ok = _boot_banner_check(monitor_log, meta)
            details.extend(banner_lines)
            if not banner_ok:
                verify_failed = True
    rc = 1 if verify_failed else 0
    if verify_failed:
        # The flash command itself succeeded; keep that fact separate from a
        # boot/identity/monitor verification failure.
        lines = ["[FAIL] device flash verify (image written)", *details]
    else:
        lines = ["[PASS] device flash", *details]
    return _finish(lines, log, "device flash", port, rc,
                   time.monotonic() - started)


def _cmd_device_smoke(args: argparse.Namespace) -> int:
    if args.seconds <= 0:
        raise UsageError("--seconds must be greater than zero")
    port = _device_port(args.port, required=True)
    status = _device_status_data(
        argparse.Namespace(port=port, probe=False, json=True))
    if not status.get("build_ready"):
        raise UsageError(
            "local firmware build is incomplete; run `mt.py build idf` first")
    meta = _device_build_meta()
    elf = Path(str(meta.get("app_elf") or ROOT / "build/microtech.elf"))
    if not elf.is_file():
        raise UsageError(f"ELF not found: {_rel(elf)}; build the firmware first")
    log = _new_log("device-smoke")
    started = time.monotonic()
    rc, seconds, _ = _run_with_input(
        _device_monitor_command(port, True, elf), log, args.seconds + 10,
        f"expect --timeout {args.seconds} boot id=\n",
    )
    rc = _normalize_monitor_rc(rc, log)
    lines = _device_monitor_lines(rc, log, "boot id=")
    if rc == 0 and lines and lines[0].startswith("[PASS]"):
        banner_lines, banner_ok = _boot_banner_check(log, meta)
        lines = ["[PASS] device smoke" if banner_ok else "[FAIL] device smoke",
                 *banner_lines]
        if not banner_ok:
            rc = 1
    elif rc == 0:
        rc = 1
    return _finish(lines, log, "device smoke", port, rc, started)


def _doctor_check(check_id: str, status: str, detail: str,
                  required: bool = True) -> dict[str, object]:
    return {
        "id": check_id,
        "status": status,
        "detail": detail,
        "required": required,
    }


def _doctor_checks() -> list[dict[str, object]]:
    checks: list[dict[str, object]] = []
    checks.append(_doctor_check(
        "python",
        "pass" if sys.version_info >= (3, 10) else "fail",
        f"{sys.version.split()[0]} (need >= 3.10)",
    ))
    idf_path = os.environ.get("IDF_PATH")
    if idf_path and Path(idf_path).is_dir():
        version = _tool_version("idf.py")
        checks.append(_doctor_check(
            "idf",
            "pass" if "v6.1" in version else "fail",
            f"{version} ({_rel(Path(idf_path))})",
        ))
    else:
        checks.append(_doctor_check("idf", "fail", "IDF_PATH is unset or invalid"))

    for tool, minimum in (("cmake", "3.22"), ("ninja", ""), ("astyle", "3.6.19")):
        version = _tool_version(tool)
        if version == "unknown":
            checks.append(_doctor_check(tool, "fail", f"{tool} not found"))
            continue
        if tool == "astyle" and minimum not in version:
            checks.append(_doctor_check(tool, "fail", f"{version} (need {minimum})"))
        elif tool == "cmake":
            match = re.search(r"(\d+)\.(\d+)", version)
            actual = (int(match.group(1)), int(match.group(2))) if match else (0, 0)
            wanted = tuple(int(part) for part in minimum.split("."))
            checks.append(_doctor_check(
                tool, "pass" if actual >= wanted else "fail",
                f"{version} (need >= {minimum})",
            ))
        else:
            checks.append(_doctor_check(tool, "pass", version))

    try:
        result = subprocess.run(
            ["git", "-C", str(ROOT), "submodule", "status", "--recursive"],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=10, check=False,
        )
        if result.returncode != 0:
            checks.append(_doctor_check(
                "submodules", "fail",
                (result.stdout or "git submodule status failed").strip()))
        else:
            missing = [line for line in result.stdout.splitlines()
                       if line.startswith("-")]
            checks.append(_doctor_check(
                "submodules",
                "pass" if not missing else "fail",
                "all initialized" if not missing
                else f"{len(missing)} uninitialized",
            ))
    except (OSError, subprocess.SubprocessError) as error:
        checks.append(_doctor_check("submodules", "fail", str(error)))
    checks.append(_doctor_check(
        "device-link-contract",
        "pass" if (ROOT / "contracts/device_link/.git").exists() else "fail",
        "contracts/device_link initialized"
        if (ROOT / "contracts/device_link/.git").exists()
        else "run git submodule update --init --recursive",
    ))

    sdkconfig = ROOT / "build/config/sdkconfig.h"
    checks.append(_doctor_check(
        "sdkconfig",
        "pass" if sdkconfig.is_file() else "fail",
        "build/config/sdkconfig.h present"
        if sdkconfig.is_file() else "run idf.py build once",
    ))

    sdl = _tool_version("sdl2-config")
    if sdl == "unknown":
        try:
            sdl = subprocess.run(
                ["pkg-config", "--modversion", "sdl2"], text=True,
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                timeout=5, check=False,
            ).stdout.strip() or "unknown"
        except (OSError, subprocess.SubprocessError):
            pass
    checks.append(_doctor_check(
        "sdl2",
        "pass" if sdl != "unknown" else "fail",
        sdl,
    ))
    curl = _tool_version("curl-config")
    if curl == "unknown":
        try:
            curl = subprocess.run(
                ["pkg-config", "--modversion", "libcurl"], text=True,
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                timeout=5, check=False,
            ).stdout.strip() or "unknown"
        except (OSError, subprocess.SubprocessError):
            pass
    checks.append(_doctor_check(
        "libcurl",
        "pass" if curl != "unknown" else "fail",
        curl,
    ))
    try:
        import ctypes.util
        sndfile = ctypes.util.find_library("sndfile") or "not found"
        checks.append(_doctor_check(
            "libsndfile",
            "pass" if sndfile != "not found" else "warn",
            sndfile,
            required=False,
        ))
    except Exception as error:  # optional runtime library
        checks.append(_doctor_check("libsndfile", "warn", str(error),
                                    required=False))
    checks.append(_doctor_check(
        "pillow",
        "pass" if importlib.util.find_spec("PIL") is not None else "fail",
        "Pillow importable" if importlib.util.find_spec("PIL") is not None
        else "pip install -r requirements-weather-assets.txt",
    ))

    for module, required in (("esp_idf_monitor", True),
                             ("esp_coredump", True),
                             ("esptool", True)):
        found = importlib.util.find_spec(module) is not None
        checks.append(_doctor_check(
            module,
            "pass" if found else ("fail" if required else "warn"),
            "importable" if found else "not installed",
            required=required,
        ))

    ports = _device_serial_ports()
    preferred_ports = _device_preferred_ports(ports)
    checks.append(_doctor_check(
        "serial-ports",
        "pass" if preferred_ports else "warn",
        "no USB serial port detected" if not preferred_ports
        else ", ".join(item["device"] for item in preferred_ports),
        required=False,
    ))
    for path in (LOG_ROOT, ROOT / "build"):
        try:
            path.mkdir(parents=True, exist_ok=True)
            probe = path / f".mt-write-probe-{os.getpid()}"
            probe.write_bytes(b"")
            probe.unlink()
            checks.append(_doctor_check(
                f"writable:{_rel(path)}", "pass", "writable"))
        except OSError as error:
            checks.append(_doctor_check(
                f"writable:{_rel(path)}", "fail", str(error)))
    return checks


def _cmd_doctor(args: argparse.Namespace) -> int:
    started = time.monotonic()
    checks = _doctor_checks()
    failed = [item for item in checks
              if item["required"] and item["status"] == "fail"]
    ok = not failed
    failure_label: str | None = None
    failure_signature: str | None = None
    repeat_count = 0
    loop_line: str | None = None
    if not ok:
        first = failed[0]
        subject = f"doctor: {first['id']}: {first['detail']}"
        failure_label = _normalize_failure(subject, keep_numbers=True) or None
        failure_signature = _failure_signature([subject])
        repeat_count, previous = _failure_streak(
            "doctor", "", failure_signature
        )
        loop_line = _loop_hint("doctor", "", repeat_count, previous)
    if args.json:
        print(json.dumps({"ok": ok, "checks": checks},
                         ensure_ascii=False, indent=1))
    else:
        print("[PASS] doctor" if ok else "[FAIL] doctor")
        if loop_line:
            print(loop_line)
        for item in checks:
            print(f"  [{str(item['status']).upper()}] {item['id']}: "
                  f"{item['detail']}")
    _append_stats({
        "timestamp": datetime.now().isoformat(timespec="seconds"),
        "command": "doctor",
        "suite": "",
        "rc": 0 if ok else 1,
        "seconds": round(time.monotonic() - started, 3),
        "raw_bytes": 0,
        "emitted_bytes": 0,
        "log": "",
        "failure_signature": failure_signature,
        "failure_label": failure_label,
        "repeat_count": repeat_count,
    })
    return 0 if ok else 1


def _cmd_device(args: argparse.Namespace) -> int:
    return int(args.device_func(args))


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
    if args.coredump and args.target != "idf":
        raise UsageError("--coredump is only valid with `build idf`")
    if args.target == "idf":
        rc, lines, log, _ = _build_idf(args.clean, args.timeout,
                                       coredump=args.coredump)
    elif args.target == "sim":
        rc, lines, log, _ = _build_sim_gate(args.clean, args.timeout)
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
    if args.clean and args.no_build:
        raise UsageError("--clean and --no-build cannot be combined")
    started = time.monotonic()
    rc, lines, log, _ = _test_host(
        suite, args.profile, args.clean, args.timeout,
        args.filter, args.repeat, args.full, args.no_build,
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


def _require_reusable_sim_build() -> None:
    """Validate the reusable scenario build before skipping its build gate."""
    build = ROOT / "build/sim"
    artifact = build / "microtech_sim"
    if not artifact.is_file() or not os.access(artifact, os.X_OK):
        raise UsageError(
            "build/sim/microtech_sim is missing; run `mt.py sim` without "
            "--no-build first"
        )
    if not (build / "sim_res_fs").is_dir():
        raise UsageError(
            "build/sim/sim_res_fs is missing; run `mt.py sim` without "
            "--no-build first"
        )
    if not (build / "gen_inc/sdkconfig.h").is_file():
        raise UsageError(
            "build/sim/gen_inc/sdkconfig.h is missing; run `mt.py sim` "
            "without --no-build first"
        )
    cache = build / "CMakeCache.txt"
    product = ""
    try:
        match = re.search(
            r"^SIM_PRODUCT:STRING=(.*)$",
            cache.read_text(encoding="utf-8", errors="replace"),
            re.MULTILINE,
        )
        if match:
            product = match.group(1).strip()
    except OSError:
        pass
    expected_product = os.environ.get("SIM_PRODUCT", "s3_reference").strip()
    if not expected_product:
        expected_product = "s3_reference"
    if product != expected_product:
        raise UsageError(
            f"build/sim is configured for '{product or 'unknown'}', but "
            f"the scenario run expects '{expected_product}'; rebuild without "
            "--no-build or set SIM_PRODUCT to match"
        )
    if not _context_matches_artifact(
            build / "build-context.json", artifact):
        raise UsageError(
            "build/sim/build-context.json does not match the current "
            "binary; run `mt.py sim` without --no-build first"
        )


def _cmd_sim(args: argparse.Namespace) -> int:
    if not args.no_build:
        _require_sim_env()
    started = time.monotonic()
    _prune_run_dirs("*-sim", 10)
    if args.no_build:
        _require_reusable_sim_build()
        gate_lines: list[str] = []
        gate_log = None
    else:
        rc, gate_lines, gate_log, _ = _build_sim_gate(False, args.timeout)
        if rc != 0:
            return _finish(
                gate_lines, gate_log, "sim gate", "", rc, started, args.full
            )
    log = _new_log("sim")
    log_dir = LOG_ROOT / f"{_timestamp()}-sim"
    log_dir.mkdir(parents=True, exist_ok=True)
    cmd = [
        "sh", str(ROOT / "sim/ci/run_ci.sh"), "--log-dir", str(log_dir),
    ]
    if args.no_build:
        cmd.append("--no-build")
    if args.jobs != 1:
        cmd.extend(["--jobs", str(args.jobs)])
    if args.keep_going:
        cmd.append("--keep-going")
    for scenario in args.scenario:
        cmd.extend(["--scenario", scenario])
    rc, seconds, _ = _run(cmd, log, args.timeout)
    lines = _sim_summary(rc, seconds, log, log_dir)
    if rc == 0:
        if gate_lines:
            lines.insert(0, gate_lines[0])
        else:
            lines.insert(0, "build reused: build/sim")
    return _finish(
        lines, log, "sim", ",".join(args.scenario) or "all", rc, started,
        args.full,
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
            "build/sim/microtech_sim is missing; run `mt.py ui` without "
            "--no-build first"
        )
    if not args.no_build:
        rc, gate_lines, gate_log, _ = _build_sim_gate(False, args.timeout)
        if rc != 0:
            return _finish(
                gate_lines, gate_log, "ui gate", "", rc, started, args.full
            )
        rc, lines, build_log, _ = _build_sim(False, args.timeout)
        if rc != 0:
            return _finish(
                lines, build_log, "ui build", "sim", rc, started, args.full
            )
    shots = LOG_ROOT / f"{_timestamp()}-ui-shots"
    shots.mkdir(parents=True, exist_ok=True)
    context_source = ROOT / "build/sim/build-context.json"
    context_artifact = ROOT / "build/sim/microtech_sim"
    if not _context_matches_artifact(context_source, context_artifact):
        _build_context_entry(ROOT / "build/sim", context_artifact, "sim")
    if (context_source.is_file() and
            _context_matches_artifact(context_source, context_artifact)):
        with contextlib.suppress(OSError):
            shutil.copy2(context_source, shots / "build-context.json")
    report = shots / "report.json"
    log = _new_log("ui")
    cmd = [
        sys.executable, str(ROOT / "sim/tools/review_pages.py"),
        "--port", "0", "--report", str(report),
        "--shots", str(shots),
    ]
    if args.pages:
        cmd.extend(["--pages", args.pages])
    for state in args.state:
        cmd.extend(["--state", state])
    rc, seconds, _ = _run(cmd, log, args.timeout)
    return _finish(
        _ui_summary(rc, seconds, report, shots, log),
        log, "ui", args.pages or "all", rc, started, args.full,
    )


def _cmd_inspect(args: argparse.Namespace) -> int:
    logs = sorted(LOG_ROOT.glob("*.log"), key=_mtime)
    if args.log:
        path = Path(args.log).expanduser()
        if not path.is_absolute():
            path = ROOT / path
    elif logs:
        path = logs[-1]
    else:
        print("[ENV] no agent logs yet")
        return 2
    if not path.is_file():
        print(f"[ENV] log not found: {path}")
        return 2
    text = _read_body(path)
    lines = text.splitlines()
    pattern = None
    if args.grep:
        try:
            pattern = re.compile(args.grep)
        except re.error as error:
            print(f"[ENV] invalid --grep expression: {error}")
            return 2
        lines = [line for line in lines if pattern.search(line)]
    category = _classify_build_error("\n".join(lines))
    blocks = _diagnostic_blocks(text, pattern)
    if args.context:
        blocks = blocks[:max(1, args.context)]
    first_error = next(
        (line for line in lines if ERROR_RE.search(line)), "unknown",
    )
    data = {
        "category": category,
        "first_error": first_error,
        "diagnostics": blocks,
        "log": _rel(path),
    }
    if args.json:
        print(json.dumps(data, ensure_ascii=False, indent=1))
        return 0
    print(f"category: {category}")
    print(f"first error: {first_error}")
    print("diagnostics:")
    for line in blocks:
        print(f"  {line}")
    print(f"log: {_rel(path)}")
    return 0


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
    entries = _load_stats()
    if not entries:
        print("[ENV] no stats yet")
        return 2
    since = getattr(args, "since", None)
    if since:
        if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", since):
            print("[ENV] --since must be YYYY-MM-DD")
            return 2
        try:
            datetime.strptime(since, "%Y-%m-%d")
        except ValueError:
            print("[ENV] --since must be YYYY-MM-DD")
            return 2
        entries = [
            entry for entry in entries
            if str(entry.get("timestamp", ""))[:10] >= since
        ]
    last = getattr(args, "last", 0)
    if last:
        entries = entries[-last:]
    failures = [
        entry for entry in entries
        if int(entry.get("rc", 0) or 0) != 0
    ]
    raw = sum(int(entry.get("raw_bytes", 0) or 0) for entry in entries)
    emitted = sum(int(entry.get("emitted_bytes", 0) or 0) for entry in entries)
    ratio = (1 - emitted / raw) * 100 if raw else 0.0
    legacy = sum(
        1 for entry in failures if not entry.get("failure_signature")
    )
    groups: list[dict[str, object]] = []
    if getattr(args, "failures", False):
        grouped: dict[tuple[str, str, str], dict[str, object]] = {}
        for entry in failures:
            command = str(entry.get("command", ""))
            suite = str(entry.get("suite", ""))
            signature = str(entry.get("failure_signature") or "")
            key = (command, suite, signature)
            record = grouped.get(key)
            if record is None:
                record = {
                    "command": command,
                    "suite": suite,
                    "signature": signature or None,
                    "label": str(entry.get("failure_label") or
                                 ("(legacy: no signature)" if not signature
                                  else "(unlabeled)")),
                    "count": 0,
                    "first": "",
                    "last": "",
                    "last_log": "",
                }
                grouped[key] = record
            record["count"] = int(record["count"]) + 1
            timestamp = str(entry.get("timestamp", ""))
            if not str(record["first"]) or timestamp < str(record["first"]):
                record["first"] = timestamp
            if not str(record["last"]) or timestamp >= str(record["last"]):
                record["last"] = timestamp
                record["last_log"] = str(entry.get("log") or "")
        groups = list(grouped.values())
        groups.sort(key=lambda item: str(item["last"]), reverse=True)
        groups.sort(key=lambda item: int(item["count"]), reverse=True)

    if getattr(args, "json", False):
        print(json.dumps(
            {
                "runs": len(entries),
                "failures": len(failures),
                "legacy": legacy,
                "raw_bytes": raw,
                "emitted_bytes": emitted,
                "reduction": round(ratio, 1),
                "groups": groups,
            },
            ensure_ascii=False,
            indent=1,
        ))
        return 0

    print(
        f"runs={len(entries)} failures={len(failures)} raw={raw} B "
        f"emitted={emitted} B reduction={ratio:.1f}%"
    )
    if getattr(args, "failures", False):
        print(
            f"[FAILURES] failed={len(failures)} groups={len(groups)} "
            f"legacy={legacy}"
        )
        for item in groups[:FAILURE_GROUP_LIMIT]:
            label = str(item["label"]).replace("\n", " ")[:FAILURE_LABEL_MAX]
            print(
                f"  {item['count']}x {item['command']} {item['suite']} "
                f"{item['first']} -> {item['last']} \"{label}\""
            )
        if len(groups) > FAILURE_GROUP_LIMIT:
            print(f"  ... {len(groups) - FAILURE_GROUP_LIMIT} more groups")
    else:
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

    build = sub.add_parser(
        "build", help="build a host suite, idf, or all simulator configurations"
    )
    build.add_argument("target")
    build.add_argument("--profile", default="none",
                       choices=("none", "address", "thread"))
    build.add_argument("--clean", action="store_true")
    build.add_argument("--coredump", action="store_true",
                       help="build idf with sdkconfig.coredump.defaults overlay")
    _add_common(build)
    build.set_defaults(func=_cmd_build)

    test = sub.add_parser("test", help="build and run a host suite")
    test.add_argument("suite")
    test.add_argument("--profile", default="none",
                      choices=("none", "address", "thread"))
    test.add_argument("--clean", action="store_true")
    test.add_argument("--no-build", action="store_true",
                      help="run CTest in the existing build without rebuilding")
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
    sim.add_argument("--no-build", action="store_true",
                     help="reuse build/sim and its build context")
    sim.add_argument("--jobs", type=int, choices=range(1, 9), default=1,
                     help="run up to N scenarios concurrently")
    sim.add_argument("--keep-going", action="store_true",
                     help="continue after a scenario failure for diagnosis")
    _add_common(sim)
    sim.set_defaults(func=_cmd_sim, timeout=1800)

    ui = sub.add_parser("ui", help="run page review")
    ui.add_argument("--pages", default=None)
    ui.add_argument("--state", action="append", default=[],
                    help="review only this state name (repeatable)")
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

    inspect = sub.add_parser(
        "inspect", help="extract the first build error and its context")
    inspect.add_argument("--last", action="store_true",
                         help="read the most recent log (default)")
    inspect.add_argument("--log", default=None)
    inspect.add_argument("--grep", default=None)
    inspect.add_argument("--context", type=int, default=0)
    inspect.add_argument("--json", action="store_true")
    inspect.set_defaults(func=_cmd_inspect)

    doctor = sub.add_parser("doctor", help="check the local development environment")
    doctor.add_argument("--json", action="store_true")
    doctor.set_defaults(func=_cmd_doctor)

    device = sub.add_parser("device", help="inspect and debug a connected board")
    device_sub = device.add_subparsers(dest="device_command", required=True)

    device_status = device_sub.add_parser("status", help="show build and board identity")
    device_status.add_argument("--port", default=None)
    device_status.add_argument("--probe", action="store_true",
                               help="read MAC/chip ID; this resets the board")
    device_status.add_argument("--json", action="store_true")
    device_status.set_defaults(device_func=_cmd_device_status)

    device_logs = device_sub.add_parser("logs", help="capture device logs")
    device_logs.add_argument("--port", default=None)
    log_mode = device_logs.add_mutually_exclusive_group()
    log_mode.add_argument("--snapshot", action="store_true",
                          help="capture and exit (default)")
    log_mode.add_argument("--follow", action="store_true",
                          help="stream until interrupted")
    device_logs.add_argument("--seconds", type=int, default=5)
    device_logs.add_argument("--timeout", type=int, default=None,
                             help="capture timeout in seconds; overrides the "
                                  "snapshot default, follow is otherwise unbounded")
    device_logs.add_argument("--reset", action="store_true",
                             help="reset the board when the monitor starts")
    device_logs.add_argument("--grep", default=None)
    device_logs.set_defaults(device_func=_cmd_device_logs)

    device_monitor = device_sub.add_parser(
        "monitor", help="run a non-interactive esp-idf-monitor command stream")
    device_monitor.add_argument("--port", default=None)
    device_monitor.add_argument("--expect", required=True,
                                help="regular expression to wait for")
    device_monitor.add_argument("--timeout", type=int, default=20)
    device_monitor.add_argument("--reset", action="store_true")
    device_monitor.set_defaults(device_func=_cmd_device_monitor)

    device_crash = device_sub.add_parser("crash", help="decode the flash coredump")
    device_crash.add_argument("--port", default=None)
    device_crash.add_argument("--save-core", default=None)
    device_crash.add_argument("--timeout", type=int, default=300)
    device_crash.set_defaults(device_func=_cmd_device_crash)

    device_flash = device_sub.add_parser("flash", help="flash after explicit --yes")
    device_flash.add_argument("--port", default=None)
    device_flash.add_argument("--yes", action="store_true",
                              help="confirm the explicit flash request")
    device_flash.add_argument("--monitor", action="store_true")
    device_flash.add_argument("--monitor-timeout", type=int, default=20,
                              help="seconds to wait for the boot banner with --monitor")
    device_flash.add_argument("--timeout", type=int, default=600)
    device_flash.set_defaults(device_func=_cmd_device_flash)

    device_smoke = device_sub.add_parser(
        "smoke", help="reset and verify the boot banner")
    device_smoke.add_argument("--port", default=None)
    device_smoke.add_argument("--seconds", type=int, default=12)
    device_smoke.set_defaults(device_func=_cmd_device_smoke)

    device.set_defaults(func=_cmd_device)

    stats = sub.add_parser("stats", help="show wrapper output statistics")
    stats.add_argument("--last", type=int, default=0)
    stats.add_argument("--failures", action="store_true",
                       help="group repeated failures by signature")
    stats.add_argument("--since", default=None,
                       help="only include entries on or after YYYY-MM-DD")
    stats.add_argument("--json", action="store_true",
                       help="print machine-readable statistics")
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
