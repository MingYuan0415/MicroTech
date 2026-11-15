#!/usr/bin/env python3
"""Build-context manifest for simulator and firmware artefacts.

The manifest is intentionally a build-directory artifact, not a second source
of project configuration. It records the binary hash that review/scenario
evidence actually exercised.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from datetime import datetime
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _sha256_text(path: Path) -> str:
    if not path.is_file():
        return ""
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _git_state() -> tuple[str, bool]:
    try:
        head = subprocess.check_output(
            ["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"],
            text=True, stderr=subprocess.DEVNULL, timeout=5,
        ).strip()
        dirty = bool(subprocess.run(
            ["git", "-C", str(ROOT), "status", "--porcelain",
             "--untracked-files=all"],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            timeout=5, check=False,
        ).stdout.strip())
        return head, dirty
    except (OSError, subprocess.SubprocessError):
        return "unknown", False


def _field(text: str, name: str) -> str:
    match = re.search(r'^#define\s+%s\s+"([^"]*)"' % re.escape(name),
                      text, re.MULTILINE)
    return match.group(1) if match else ""


def _config_path(build_dir: Path) -> Path:
    for relative in ("config/sdkconfig.h", "gen_inc/sdkconfig.h"):
        candidate = build_dir / relative
        if candidate.is_file():
            return candidate
    return build_dir / "config/sdkconfig.h"


def _target(build_dir: Path, profile_text: str) -> str:
    platform = _field(profile_text, "MT_PRODUCT_DEVICE_PLATFORM")
    if platform:
        normalized = re.sub(r"[^a-z0-9]", "", platform.lower())
        if normalized.startswith("esp32"):
            return normalized
    config = _config_path(build_dir)
    if not config.is_file():
        return ""
    match = re.search(r'^#define CONFIG_IDF_TARGET "([^"]+)"',
                      config.read_text(encoding="utf-8", errors="replace"),
                      re.MULTILINE)
    return match.group(1) if match else ""


def _idf_version() -> str:
    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        return ""
    for candidate in (Path(idf_path) / "version.txt",
                      Path(idf_path) / "VERSION"):
        if candidate.is_file():
            return candidate.read_text(encoding="utf-8",
                                       errors="replace").strip()
    try:
        output = subprocess.check_output(
            ["idf.py", "--version"], text=True,
            stderr=subprocess.STDOUT, timeout=10,
        )
        match = re.search(r"ESP-IDF\s+(\S+)", output)
        return match.group(1) if match else output.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def collect(build_dir: Path, artifact: Path, kind: str) -> dict[str, object]:
    build_dir = build_dir.resolve()
    artifact = artifact.resolve()
    if not artifact.is_file():
        raise FileNotFoundError(artifact)
    profile = build_dir / "mt_product_profile.h"
    if not profile.is_file():
        raise FileNotFoundError(profile)
    profile_text = profile.read_text(encoding="utf-8", errors="replace")
    product = _field(profile_text, "MT_PRODUCT_NAME")
    panel_text = _field(profile_text, "MT_PRODUCT_DEVICE_PANEL")
    panel_match = re.search(r"(\d+)\s*[xX]\s*(\d+)", panel_text)
    if not product or panel_match is None:
        raise ValueError(f"incomplete product profile: {profile}")
    config_path = _config_path(build_dir)
    if not config_path.is_file():
        raise FileNotFoundError(config_path)
    git_head, git_dirty = _git_state()
    context: dict[str, object] = {
        "schema": 1,
        "kind": kind,
        "generated_at": datetime.now().astimezone().isoformat(
            timespec="seconds"),
        "git_head": git_head,
        "git_dirty": git_dirty,
        "idf_version": _idf_version(),
        "target": _target(build_dir, profile_text),
        "product": product,
        "panel": {
            "width": int(panel_match.group(1)) if panel_match else 0,
            "height": int(panel_match.group(2)) if panel_match else 0,
        },
        "build_dir": str(build_dir.relative_to(ROOT))
        if build_dir.is_relative_to(ROOT) else str(build_dir),
        "artifact": str(artifact.relative_to(ROOT))
        if artifact.is_relative_to(ROOT) else str(artifact),
        "artifact_sha256": _sha256(artifact),
        "artifact_size": artifact.stat().st_size,
        "config_sha256": _sha256_text(config_path),
    }
    return context


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True)
    parser.add_argument("--artifact", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--kind", default="sim")
    args = parser.parse_args()
    try:
        context = collect(Path(args.build), Path(args.artifact), args.kind)
    except (OSError, ValueError) as error:
        print(f"build-context: {error}", file=sys.stderr)
        return 2
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(context, ensure_ascii=False, indent=1) + "\n",
                   encoding="utf-8")
    print(json.dumps(context, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
