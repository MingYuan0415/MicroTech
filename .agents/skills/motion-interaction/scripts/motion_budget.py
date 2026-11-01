#!/usr/bin/env python3
"""Validate an interaction/motion plan against a resolved motion budget."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

ALLOWED_MECHANISMS = {"none", "style_transition", "lv_anim"}
ALLOWED_PROPERTIES = {"color", "opacity", "translate_x", "translate_y", "scale"}
FORBIDDEN_PROPERTIES = {
    "width", "height", "left", "top", "layout", "blur", "shadow",
    "background_position", "filter", "clip_path",
}
HIGH_FREQUENCY_TRIGGERS = {
    "back", "app_switch", "scroll", "key", "keyboard", "keypad",
    "list", "list_scroll", "list_navigation",
}


def _finding(code: str, severity: str, item: dict[str, Any], message: str) -> dict[str, Any]:
    return {
        "code": code,
        "severity": severity,
        "item": item.get("id", "unknown"),
        "message": message,
    }


def validate_plan(plan: dict[str, Any], context: dict[str, Any]) -> dict[str, Any]:
    resolved = context["resolved"]
    findings: list[dict[str, Any]] = []
    if plan.get("profile_id") and plan["profile_id"] != context["profile"]["id"]:
        findings.append({
            "code": "PROFILE_MISMATCH", "severity": "blocker", "item": "plan",
            "message": f"plan is {plan['profile_id']}, context is {context['profile']['id']}",
        })
    if plan.get("target_id") and plan["target_id"] != context["target"]["id"]:
        findings.append({
            "code": "TARGET_MISMATCH", "severity": "blocker", "item": "plan",
            "message": f"plan is {plan['target_id']}, context is {context['target']['id']}",
        })
    for plan_key, section, code in (
            ("profile_version", "profile", "PROFILE_VERSION_MISMATCH"),
            ("target_version", "target", "TARGET_VERSION_MISMATCH")):
        value = plan.get(plan_key)
        if value is None:
            continue
        if isinstance(value, bool) or not isinstance(value, int) or value < 1:
            findings.append({
                "code": f"INVALID_{plan_key.upper()}", "severity": "blocker",
                "item": "plan", "message": f"{plan_key} must be a positive integer",
            })
        elif value != context[section]["version"]:
            findings.append({
                "code": code, "severity": "blocker", "item": "plan",
                "message": f"plan {plan_key} {value} != context "
                           f"{context[section]['version']}",
            })

    level = resolved["motion_level"]
    max_ms = int(resolved["max_motion_ms"])
    max_objects = int(resolved["max_animated_objects"])
    items = plan.get("items") or []
    if not isinstance(items, list):
        findings.append({"code": "INVALID_PLAN", "severity": "blocker", "item": "plan",
                         "message": "items must be a list"})
        return {"profile_id": context["profile"]["id"],
                "profile_version": context["profile"]["version"],
                "target_id": context["target"]["id"],
                "target_version": context["target"]["version"],
                "motion_level": level, "findings": findings}

    if level == "none" and any(
            not isinstance(item, dict) or item.get("mechanism", "none") != "none"
            for item in items):
        findings.append({"code": "MOTION_NOT_ALLOWED", "severity": "blocker",
                         "item": "plan", "message": "target allows instant state changes only"})

    total_objects = 0
    for item in items:
        if isinstance(item, dict):
            default_objects = 0 if item.get("mechanism", "none") == "none" else 1
            objects = item.get("objects", default_objects)
            if isinstance(objects, int):
                total_objects += max(0, objects)
    if total_objects > max_objects:
        findings.append({"code": "TOTAL_OBJECTS_EXCEEDS_BUDGET", "severity": "blocker",
                         "item": "plan",
                         "message": f"{total_objects} objects > {max_objects} concurrent budget"})

    for item in items:
        if not isinstance(item, dict):
            findings.append({"code": "INVALID_ITEM", "severity": "blocker",
                             "item": "unknown", "message": "item must be an object"})
            continue
        mechanism = item.get("mechanism", "none")
        if mechanism not in ALLOWED_MECHANISMS:
            findings.append(_finding("MECHANISM_NOT_ALLOWED", "blocker", item,
                                     f"mechanism {mechanism!r} is not allowed"))
        duration = item.get("duration_ms", 0)
        objects = item.get("objects", 0 if mechanism == "none" else 1)
        if not isinstance(duration, (int, float)) or duration < 0:
            findings.append(_finding("INVALID_DURATION", "blocker", item, "duration_ms must be non-negative"))
        elif duration > max_ms:
            findings.append(_finding("DURATION_EXCEEDS_BUDGET", "blocker", item,
                                     f"{duration}ms > {max_ms}ms"))
        if not isinstance(objects, int) or objects < 0:
            findings.append(_finding("INVALID_OBJECT_COUNT", "blocker", item, "objects must be a non-negative integer"))
        elif objects > max_objects:
            findings.append(_finding("TOO_MANY_OBJECTS", "blocker", item,
                                     f"{objects} objects > {max_objects}"))
        if mechanism == "none":
            if duration != 0:
                findings.append(_finding("INSTANT_DURATION_NONZERO", "blocker", item,
                                         "mechanism none requires duration_ms 0"))
            if objects != 0:
                findings.append(_finding("INSTANT_OBJECTS_NONZERO", "blocker", item,
                                         "mechanism none requires objects 0"))
        else:
            prop = item.get("property")
            if prop in FORBIDDEN_PROPERTIES:
                findings.append(_finding("PROPERTY_NOT_ALLOWED", "blocker", item,
                                         f"property {prop!r} is not animation-safe"))
            elif prop not in ALLOWED_PROPERTIES:
                findings.append(_finding("PROPERTY_UNKNOWN", "major", item,
                                         f"property {prop!r} is not in the safe set"))
        if item.get("loop") is True:
            findings.append(_finding("LOOP_NOT_ALLOWED", "blocker", item,
                                     "infinite or looping motion is forbidden"))
        trigger = str(item.get("trigger", "")).lower()
        if trigger in HIGH_FREQUENCY_TRIGGERS and mechanism != "none":
            findings.append(_finding("HIGH_FREQUENCY_ANIMATION", "blocker", item,
                                     f"{trigger} is high-frequency and must be instant"))

    return {
        "profile_id": context["profile"]["id"],
        "profile_version": context["profile"]["version"],
        "target_id": context["target"]["id"],
        "target_version": context["target"]["version"],
        "motion_level": level,
        "limits": {"max_motion_ms": max_ms,
                   "max_animated_objects": max_objects,
                   "total_objects": total_objects},
        "findings": findings,
        "summary": {
            "blockers": sum(1 for item in findings if item["severity"] == "blocker"),
            "major": sum(1 for item in findings if item["severity"] == "major"),
        },
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", required=True)
    parser.add_argument("--context", required=True)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    plan = json.loads(Path(args.plan).read_text(encoding="utf-8"))
    context = json.loads(Path(args.context).read_text(encoding="utf-8"))
    result = validate_plan(plan, context)
    print(json.dumps(result, indent=2))
    if args.check and result["findings"]:
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
