#!/usr/bin/env python3
"""Resolve and validate MicroTech design profiles and target profiles.

The design contract is intentionally independent of the current panel and the
current visual language. A style profile describes a visual language; a target
profile describes capabilities and budgets. Only `resolve()` combines them.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
from pathlib import Path
from typing import Any

SCHEMA_VERSION = 1

STYLE_AXES = {
    "surface_model": {"flat", "tonal", "elevated", "outlined"},
    "depth_model": {"none", "border", "lightness", "shadow"},
    "density": {"compact", "comfortable", "spacious"},
    "corner_scale": {"square", "soft", "rounded", "pill"},
    "type_scale": {"compact", "standard", "editorial"},
    "decoration": {"none", "sparse", "expressive"},
    "motion_level": {"none", "micro", "standard", "rich"},
    "contrast_profile": {"normal", "high", "maximum"},
    "icon_style": {"outline", "filled", "geometric"},
    "color_temperature": {"cool", "neutral", "warm"},
}

REQUIRED_ROLES = {
    "canvas", "surface_1", "surface_2", "surface_pressed", "surface_low",
    "border", "track", "ink", "ink_muted", "ink_faint", "accent", "on_accent",
    "success", "warning", "danger", "focus",
    "accent_tint", "success_tint", "warning_tint", "danger_tint",
}

PROFILE_KEYS = {
    "schema_version", "id", "version", "display_name", "style_axes", "roles",
    "scales", "rules", "requires", "compatibility", "fallback", "fixture",
}
TARGET_KEYS = {
    "schema_version", "id", "version", "display", "input", "resources",
    "supported", "fixture",
}
DISPLAY_KEYS = {
    "width", "height", "dpi", "color_mode", "refresh_hz", "orientation",
    "emissive",
}
INPUT_KEYS = {"touch", "keypad", "encoder", "min_touch_mm"}
RESOURCE_KEYS = {
    "cpu_class", "ram_class", "base_space_px", "base_radius_px",
    "base_font_px", "available_font_px", "typography_tolerance_px",
    "max_motion_level", "max_motion_ms", "max_animated_objects",
    "allow_blur", "allow_shadow", "allow_gradients", "max_image_bytes",
    "max_icon_count",
}
SCALE_KEYS = {"space", "radius", "type"}
RULE_KEYS = {
    "min_touch_mm", "min_contrast_body", "min_contrast_large",
    "min_contrast_ui", "min_contrast_faint", "max_accent_roles",
    "allow_gradients", "allow_shadow", "allow_blur", "contrast_pairs",
}
COMPATIBILITY_KEYS = {"colors", "metric_checks", "app_aliases"}
TYPE_ROLES = ("caption", "body", "title", "heading", "display", "hero")
CONTRAST_FLOORS = {
    "body": "min_contrast_body",
    "large": "min_contrast_large",
    "ui": "min_contrast_ui",
    "faint": "min_contrast_faint",
}

MOTION_RANK = {"none": 0, "micro": 1, "standard": 2, "rich": 3}
MOTION_DURATION_CAP = {"none": 0, "micro": 150, "standard": 250, "rich": 300}
MOTION_OBJECT_CAP = {"none": 0, "micro": 2, "standard": 4, "rich": 4}

UNIVERSAL_MIN_CONTRAST_BODY = 4.5
UNIVERSAL_MIN_CONTRAST_LARGE = 3.0
UNIVERSAL_MIN_CONTRAST_UI = 3.0
UNIVERSAL_MIN_CONTRAST_FAINT = 4.0
UNIVERSAL_MIN_TOUCH_MM = 4.5

COLOR_MODES = {"rgb565", "grayscale", "mono"}
ID_RE = re.compile(r"^[a-z][a-z0-9_]*$")
HEX_RE = re.compile(r"^#[0-9A-Fa-f]{6}$")
COLOR_DEFINE_RE = re.compile(
    r"^\s*#define\s+(APP_THEME_COLOR_[A-Z0-9_]+)\s+(0x[0-9A-Fa-f]+)",
    re.MULTILINE,
)
METRIC_DEFINE_RE = re.compile(
    r"^\s*#define\s+(APP_UI_[A-Z0-9_]+)\s+([0-9]+)",
    re.MULTILINE,
)
ALIAS_DEFINE_RE = re.compile(
    r"^\s*#define\s+(APP_UI_COLOR_[A-Z0-9_]+)\s+"
    r"(APP_THEME_COLOR_[A-Z0-9_]+)",
    re.MULTILINE,
)


class DesignProfileError(ValueError):
    """A profile/target combination is invalid."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code
        self.message = message

    def __str__(self) -> str:
        return f"{self.code}: {self.message}"


def repo_root() -> Path:
    for parent in Path(__file__).resolve().parents:
        if (parent / "doc" / "design" / "profiles").is_dir():
            return parent
    return Path.cwd()


def load_json(path: str | Path) -> dict[str, Any]:
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def _require(mapping: dict[str, Any], key: str, where: str,
             errors: list[str]) -> Any:
    if key not in mapping:
        errors.append(f"{where}: missing {key}")
        return None
    return mapping[key]


def _check_hex(value: Any, where: str, errors: list[str]) -> None:
    if not isinstance(value, str) or not HEX_RE.match(value):
        errors.append(f"{where}: expected #RRGGBB, got {value!r}")


def _srgb_channel(value: int) -> float:
    normalized = value / 255.0
    if normalized <= 0.04045:
        return normalized / 12.92
    return ((normalized + 0.055) / 1.055) ** 2.4


def relative_luminance(hex_color: str) -> float:
    value = hex_color.lstrip("#")
    red, green, blue = (int(value[index:index + 2], 16) for index in (0, 2, 4))
    return (0.2126 * _srgb_channel(red) +
            0.7152 * _srgb_channel(green) +
            0.0722 * _srgb_channel(blue))


def contrast_ratio(foreground: str, background: str) -> float:
    first = relative_luminance(foreground)
    second = relative_luminance(background)
    lighter, darker = max(first, second), min(first, second)
    return (lighter + 0.05) / (darker + 0.05)


def validate_profile(profile: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    if not isinstance(profile, dict):
        return ["profile: expected object"]
    unknown = set(profile) - PROFILE_KEYS
    if unknown:
        errors.append(f"profile: unknown fields {sorted(unknown)}")
    if profile.get("schema_version") != SCHEMA_VERSION:
        errors.append("profile: unsupported schema_version")
    profile_id = profile.get("id")
    if not isinstance(profile_id, str) or not ID_RE.match(profile_id):
        errors.append("profile: id must be lowercase snake_case")
    if not isinstance(profile.get("display_name"), str) or not profile["display_name"].strip():
        errors.append("profile: display_name must be a non-empty string")
    if not isinstance(profile.get("version"), int) or profile["version"] < 1:
        errors.append("profile: version must be a positive integer")
    for key in ("fallback", "fixture"):
        if key in profile and not isinstance(profile[key], bool):
            errors.append(f"profile: {key} must be boolean")
    if "fixture" not in profile:
        errors.append("profile: missing fixture")
    if profile.get("fallback") is True and profile.get("id") != "neutral":
        errors.append("profile: fallback is reserved for neutral")

    axes = _require(profile, "style_axes", "profile", errors)
    if isinstance(axes, dict):
        unknown_axes = set(axes) - set(STYLE_AXES)
        if unknown_axes:
            errors.append(f"style_axes: unknown axes {sorted(unknown_axes)}")
        for name, allowed in STYLE_AXES.items():
            value = axes.get(name)
            if value not in allowed:
                errors.append(f"style_axes.{name}: expected one of {sorted(allowed)}, got {value!r}")
    elif axes is not None:
        errors.append("profile: style_axes must be an object")

    roles = _require(profile, "roles", "profile", errors)
    if isinstance(roles, dict):
        missing = REQUIRED_ROLES - set(roles)
        if missing:
            errors.append(f"roles: missing {sorted(missing)}")
        unknown_roles = set(roles) - REQUIRED_ROLES
        if unknown_roles:
            errors.append(f"roles: unknown roles {sorted(unknown_roles)}")
        for name, value in roles.items():
            _check_hex(value, f"roles.{name}", errors)
    elif roles is not None:
        errors.append("profile: roles must be an object")

    scales = _require(profile, "scales", "profile", errors)
    if isinstance(scales, dict):
        unknown_scales = set(scales) - SCALE_KEYS
        if unknown_scales:
            errors.append(f"scales: unknown keys {sorted(unknown_scales)}")
        space = scales.get("space")
        if not isinstance(space, list) or len(space) < 3 or any(not isinstance(item, (int, float)) or item <= 0 for item in space):
            errors.append("scales.space: expected at least three positive numbers")
        elif any(left >= right for left, right in zip(space, space[1:])):
            errors.append("scales.space: values must be strictly increasing")
        radius = scales.get("radius")
        if not isinstance(radius, list) or len(radius) < 3 or any(not isinstance(item, (int, float)) or item < 0 for item in radius):
            errors.append("scales.radius: expected at least three non-negative numbers")
        elif any(left > right for left, right in zip(radius, radius[1:])):
            errors.append("scales.radius: values must be non-decreasing")
        type_scale = scales.get("type")
        if not isinstance(type_scale, dict):
            errors.append("scales.type: expected object")
        else:
            unknown_type_roles = set(type_scale) - set(TYPE_ROLES)
            if unknown_type_roles:
                errors.append(f"scales.type: unknown roles {sorted(unknown_type_roles)}")
            for role in TYPE_ROLES:
                value = type_scale.get(role)
                if not isinstance(value, (int, float)) or value <= 0:
                    errors.append(f"scales.type.{role}: expected positive number")
            ordered = [type_scale.get(role) for role in TYPE_ROLES]
            if all(isinstance(value, (int, float)) for value in ordered):
                if any(left >= right for left, right in zip(ordered, ordered[1:])):
                    errors.append("scales.type: roles must be strictly increasing")
    elif scales is not None:
        errors.append("profile: scales must be an object")

    rules = _require(profile, "rules", "profile", errors)
    if isinstance(rules, dict):
        unknown_rules = set(rules) - RULE_KEYS
        if unknown_rules:
            errors.append(f"rules: unknown fields {sorted(unknown_rules)}")
        floors = {
            "min_touch_mm": UNIVERSAL_MIN_TOUCH_MM,
            "min_contrast_body": UNIVERSAL_MIN_CONTRAST_BODY,
            "min_contrast_large": UNIVERSAL_MIN_CONTRAST_LARGE,
            "min_contrast_ui": UNIVERSAL_MIN_CONTRAST_UI,
            "min_contrast_faint": UNIVERSAL_MIN_CONTRAST_FAINT,
        }
        floor_values: dict[str, float] = {}
        for key, floor in floors.items():
            value = rules.get(key)
            if isinstance(value, bool) or not isinstance(value, (int, float)) or value < floor:
                errors.append(f"rules.{key}: expected number >= {floor}")
            else:
                floor_values[key] = float(value)
        max_accent = rules.get("max_accent_roles")
        if isinstance(max_accent, bool) or not isinstance(max_accent, int) or max_accent < 0:
            errors.append("rules.max_accent_roles: expected non-negative integer")
        for key in ("allow_gradients", "allow_shadow", "allow_blur"):
            if not isinstance(rules.get(key), bool):
                errors.append(f"rules.{key}: expected boolean")
        pairs = rules.get("contrast_pairs")
        if not isinstance(pairs, list) or not pairs:
            errors.append("rules.contrast_pairs: expected non-empty list")
        elif isinstance(roles, dict):
            for pair in pairs:
                floor_name = "body"
                if isinstance(pair, dict):
                    if set(pair) - {"foreground", "background", "floor"}:
                        errors.append(f"rules.contrast_pairs: unknown fields in {pair!r}")
                        continue
                    foreground = pair.get("foreground")
                    background = pair.get("background")
                    floor_name = pair.get("floor", "body")
                elif isinstance(pair, list) and len(pair) == 2:
                    foreground, background = pair
                else:
                    errors.append("rules.contrast_pairs: each entry must be an object or [foreground, background]")
                    continue
                if floor_name not in CONTRAST_FLOORS:
                    errors.append(f"rules.contrast_pairs: unknown floor {floor_name!r}")
                    continue
                if foreground not in roles or background not in roles:
                    errors.append(f"rules.contrast_pairs: unknown role in {pair!r}")
                    continue
                if HEX_RE.match(str(roles[foreground])) and HEX_RE.match(str(roles[background])):
                    ratio = contrast_ratio(roles[foreground], roles[background])
                    minimum = floor_values.get(CONTRAST_FLOORS[floor_name])
                    if minimum is not None and ratio + 0.01 < minimum:
                        errors.append(
                            f"contrast {foreground}/{background}: {ratio:.2f} < "
                            f"{minimum:.2f} ({floor_name})"
                        )
    elif rules is not None:
        errors.append("profile: rules must be an object")

    requires = _require(profile, "requires", "profile", errors)
    if isinstance(requires, dict):
        unknown_requires = set(requires) - {"color_modes", "min_refresh_hz"}
        if unknown_requires:
            errors.append(f"requires: unknown keys {sorted(unknown_requires)}")
        modes = requires.get("color_modes")
        if not isinstance(modes, list) or not modes or not set(modes) <= COLOR_MODES:
            errors.append(f"requires.color_modes: expected non-empty subset of {sorted(COLOR_MODES)}")
        if not isinstance(requires.get("min_refresh_hz"), (int, float)) or requires["min_refresh_hz"] < 0:
            errors.append("requires.min_refresh_hz: expected non-negative number")
    elif requires is not None:
        errors.append("profile: requires must be an object")

    compatibility = profile.get("compatibility")
    if compatibility is not None:
        if not isinstance(compatibility, dict):
            errors.append("compatibility: expected object")
        else:
            unknown_compatibility = set(compatibility) - COMPATIBILITY_KEYS
            if unknown_compatibility:
                errors.append(
                    f"compatibility: unknown fields {sorted(unknown_compatibility)}")
            for key in ("colors", "metric_checks", "app_aliases"):
                value = compatibility.get(key)
                if value is not None and not isinstance(value, dict):
                    errors.append(f"compatibility.{key}: expected object")
            colors_compat = compatibility.get("colors")
            if isinstance(colors_compat, dict):
                for key, value in colors_compat.items():
                    if key not in REQUIRED_ROLES or not isinstance(value, str) or not value:
                        errors.append(f"compatibility.colors.{key}: expected role -> macro")
            aliases_compat = compatibility.get("app_aliases")
            if isinstance(aliases_compat, dict):
                for key, value in aliases_compat.items():
                    if key not in REQUIRED_ROLES or not isinstance(value, str) or not value:
                        errors.append(f"compatibility.app_aliases.{key}: expected role -> macro")
    return errors


def validate_target(target: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    if not isinstance(target, dict):
        return ["target: expected object"]
    unknown = set(target) - TARGET_KEYS
    if unknown:
        errors.append(f"target: unknown fields {sorted(unknown)}")
    if target.get("schema_version") != SCHEMA_VERSION:
        errors.append("target: unsupported schema_version")
    target_id = target.get("id")
    if not isinstance(target_id, str) or not ID_RE.match(target_id):
        errors.append("target: id must be lowercase snake_case")
    if not isinstance(target.get("version"), int) or target["version"] < 1:
        errors.append("target: version must be a positive integer")

    display = _require(target, "display", "target", errors)
    if isinstance(display, dict):
        unknown_display = set(display) - DISPLAY_KEYS
        if unknown_display:
            errors.append(f"display: unknown fields {sorted(unknown_display)}")
        for key in ("width", "height", "dpi", "refresh_hz"):
            if not isinstance(display.get(key), (int, float)) or display[key] <= 0:
                errors.append(f"display.{key}: expected positive number")
        if display.get("color_mode") not in COLOR_MODES:
            errors.append(f"display.color_mode: expected one of {sorted(COLOR_MODES)}")
        orientation = display.get("orientation")
        if orientation not in {"portrait", "landscape", "square"}:
            errors.append("display.orientation: expected portrait, landscape or square")
        elif all(isinstance(display.get(key), (int, float)) for key in ("width", "height")):
            width, height = float(display["width"]), float(display["height"])
            if orientation == "portrait" and width > height:
                errors.append("display.orientation: portrait requires width <= height")
            elif orientation == "landscape" and width < height:
                errors.append("display.orientation: landscape requires width >= height")
            elif orientation == "square" and width != height:
                errors.append("display.orientation: square requires width == height")
        if "emissive" in display and not isinstance(display["emissive"], bool):
            errors.append("display.emissive: expected boolean")
    elif display is not None:
        errors.append("target: display must be an object")

    inputs = _require(target, "input", "target", errors)
    if isinstance(inputs, dict):
        unknown_inputs = set(inputs) - INPUT_KEYS
        if unknown_inputs:
            errors.append(f"input: unknown fields {sorted(unknown_inputs)}")
        for key in ("touch", "keypad", "encoder"):
            if not isinstance(inputs.get(key), bool):
                errors.append(f"input.{key}: expected boolean")
        if not isinstance(inputs.get("min_touch_mm"), (int, float)) or inputs["min_touch_mm"] < 0:
            errors.append("input.min_touch_mm: expected non-negative number")
        elif inputs.get("touch") is True and inputs["min_touch_mm"] < UNIVERSAL_MIN_TOUCH_MM:
            errors.append(
                f"input.min_touch_mm: touch targets need >= {UNIVERSAL_MIN_TOUCH_MM} mm")
    elif inputs is not None:
        errors.append("target: input must be an object")

    resources = _require(target, "resources", "target", errors)
    if isinstance(resources, dict):
        unknown_resources = set(resources) - RESOURCE_KEYS
        if unknown_resources:
            errors.append(f"resources: unknown fields {sorted(unknown_resources)}")
        for key in ("base_space_px", "base_font_px"):
            if not isinstance(resources.get(key), (int, float)) or resources[key] <= 0:
                errors.append(f"resources.{key}: expected positive number")
        for key in ("base_radius_px", "max_motion_ms", "max_animated_objects",
                    "max_image_bytes", "max_icon_count"):
            if not isinstance(resources.get(key), (int, float)) or resources[key] < 0:
                errors.append(f"resources.{key}: expected non-negative number")
        for key in ("cpu_class", "ram_class"):
            if key in resources and (not isinstance(resources[key], str) or not resources[key].strip()):
                errors.append(f"resources.{key}: expected non-empty string")
        available_fonts = resources.get("available_font_px")
        if available_fonts is not None:
            if (not isinstance(available_fonts, list) or not available_fonts or
                    any(not isinstance(value, (int, float)) or value <= 0
                        for value in available_fonts)):
                errors.append("resources.available_font_px: expected non-empty positive list")
            elif any(left >= right for left, right in zip(available_fonts, available_fonts[1:])):
                errors.append("resources.available_font_px: expected strictly increasing values")
        if not isinstance(resources.get("typography_tolerance_px", 0),
                          (int, float)) or resources.get("typography_tolerance_px", 0) < 0:
            errors.append("resources.typography_tolerance_px: expected non-negative number")
        if resources.get("max_motion_level") not in MOTION_RANK:
            errors.append("resources.max_motion_level: expected none, micro, standard or rich")
        for key in ("allow_blur", "allow_shadow", "allow_gradients"):
            if not isinstance(resources.get(key), bool):
                errors.append(f"resources.{key}: expected boolean")
    elif resources is not None:
        errors.append("target: resources must be an object")

    for key in ("supported", "fixture"):
        if not isinstance(target.get(key), bool):
            errors.append(f"target: {key} must be boolean")
    if target.get("fixture") is True and target.get("supported") is True:
        errors.append("target: fixture targets must not be marked supported")
    return errors


EXPECTED_PAIR_ERRORS = {"COLOR_MODE_UNSUPPORTED", "REFRESH_TOO_LOW"}


def validate_pairs(profiles: dict[str, dict[str, Any]],
                   targets: dict[str, dict[str, Any]]) -> dict[str, Any]:
    results: list[dict[str, Any]] = []
    failures: list[str] = []
    for profile_id in sorted(profiles):
        for target_id in sorted(targets):
            try:
                context = resolve(profiles[profile_id], targets[target_id])
                codes = [item["code"] for item in context["findings"]]
                results.append({
                    "profile": profile_id,
                    "target": target_id,
                    "status": "warning" if codes else "ok",
                    "findings": codes,
                })
            except DesignProfileError as error:
                status = "expected" if error.code in EXPECTED_PAIR_ERRORS else "error"
                results.append({
                    "profile": profile_id,
                    "target": target_id,
                    "status": status,
                    "code": error.code,
                })
                if status == "error":
                    failures.append(f"{profile_id}/{target_id}: {error.code}")
    return {"pairs": results, "failures": failures}


def _check(profile: dict[str, Any], target: dict[str, Any]) -> None:
    profile_errors = validate_profile(profile)
    target_errors = validate_target(target)
    if profile_errors or target_errors:
        details = profile_errors + target_errors
        raise DesignProfileError("INVALID_PROFILE_OR_TARGET", "; ".join(details))
    display = target["display"]
    resources = target["resources"]
    requires = profile["requires"]
    if display["color_mode"] not in requires["color_modes"]:
        raise DesignProfileError(
            "COLOR_MODE_UNSUPPORTED",
            f"{profile['id']} requires {requires['color_modes']}, "
            f"target {target['id']} provides {display['color_mode']}",
        )
    if display["refresh_hz"] < requires["min_refresh_hz"]:
        raise DesignProfileError(
            "REFRESH_TOO_LOW",
            f"{profile['id']} needs {requires['min_refresh_hz']} Hz, "
            f"target {target['id']} provides {display['refresh_hz']} Hz",
        )
    if profile["style_axes"]["motion_level"] not in MOTION_RANK:
        raise DesignProfileError("INVALID_MOTION_LEVEL", profile["style_axes"]["motion_level"])
    if resources["max_motion_level"] not in MOTION_RANK:
        raise DesignProfileError("INVALID_MOTION_LEVEL", resources["max_motion_level"])


def resolve(profile: dict[str, Any], target: dict[str, Any]) -> dict[str, Any]:
    _check(profile, target)
    display = target["display"]
    inputs = target["input"]
    resources = target["resources"]
    rules = profile["rules"]
    findings: list[dict[str, str]] = []

    style_motion = profile["style_axes"]["motion_level"]
    target_motion = resources["max_motion_level"]
    motion_level = style_motion if MOTION_RANK[style_motion] <= MOTION_RANK[target_motion] else target_motion
    if motion_level != style_motion:
        findings.append({
            "code": "MOTION_DOWNGRADED",
            "message": f"{style_motion} -> {motion_level} for {target['id']}",
        })

    allow_shadow = bool(rules["allow_shadow"] and resources["allow_shadow"])
    allow_blur = bool(rules["allow_blur"] and resources["allow_blur"])
    allow_gradients = bool(rules["allow_gradients"] and resources["allow_gradients"])
    if rules["allow_shadow"] and not allow_shadow:
        findings.append({"code": "EFFECT_DOWNGRADED", "message": "shadow disabled by target"})
    if rules["allow_blur"] and not allow_blur:
        findings.append({"code": "EFFECT_DOWNGRADED", "message": "blur disabled by target"})
    if rules["allow_gradients"] and not allow_gradients:
        findings.append({"code": "EFFECT_DOWNGRADED", "message": "gradients disabled by target"})

    touch_mm = max(float(rules["min_touch_mm"]), float(inputs["min_touch_mm"])) if inputs["touch"] else 0.0
    min_touch_px = math.ceil(touch_mm / 25.4 * float(display["dpi"])) if inputs["touch"] else 0

    def scale(values: list[float], base: float) -> list[int]:
        return [int(round(value * base)) for value in values]

    type_px = {
        role: int(round(value * resources["base_font_px"]))
        for role, value in profile["scales"]["type"].items()
    }
    available_fonts = resources.get("available_font_px")
    typography_supported = True
    if available_fonts:
        tolerance = float(resources.get("typography_tolerance_px", 0))
        unsupported = []
        for role, size in type_px.items():
            if not any(abs(size - available) <= tolerance for available in available_fonts):
                unsupported.append(f"{role}={size}px")
        if unsupported:
            typography_supported = False
            findings.append({
                "code": "TYPOGRAPHY_UNSUPPORTED",
                "message": f"{profile['id']} type roles not available: {', '.join(unsupported)}",
            })

    resolved = {
        "colors": dict(profile["roles"]),
        "space_px": scale(profile["scales"]["space"], resources["base_space_px"]),
        "radius_px": scale(profile["scales"]["radius"], resources["base_radius_px"]),
        "type_px": type_px,
        "typography_supported": typography_supported,
        "min_contrast_body": float(rules["min_contrast_body"]),
        "min_contrast_large": float(rules["min_contrast_large"]),
        "min_contrast_ui": float(rules["min_contrast_ui"]),
        "min_contrast_faint": float(rules["min_contrast_faint"]),
        "min_touch_mm": touch_mm,
        "min_touch_px": min_touch_px,
        "motion_level": motion_level,
        "max_motion_ms": min(int(resources["max_motion_ms"]),
                            MOTION_DURATION_CAP[motion_level]),
        "max_animated_objects": min(int(resources["max_animated_objects"]),
                                  MOTION_OBJECT_CAP[motion_level]),
        "allow_shadow": allow_shadow,
        "allow_blur": allow_blur,
        "allow_gradients": allow_gradients,
        "max_image_bytes": int(resources["max_image_bytes"]),
        "max_icon_count": int(resources["max_icon_count"]),
    }
    return {
        "schema_version": SCHEMA_VERSION,
        "profile": {
            "id": profile["id"],
            "version": profile["version"],
            "display_name": profile["display_name"],
            "style_axes": profile["style_axes"],
            "rules": rules,
            "requires": profile["requires"],
            "fallback": bool(profile.get("fallback", False)),
            "fixture": bool(profile["fixture"]),
        },
        "target": {
            "id": target["id"],
            "version": target["version"],
            "display": display,
            "input": inputs,
            "resources": resources,
            "supported": target["supported"],
            "fixture": target["fixture"],
        },
        "resolved": resolved,
        "findings": findings,
    }


def load_profiles(directory: str | Path) -> dict[str, dict[str, Any]]:
    output = {}
    for path in sorted(Path(directory).glob("*.json")):
        profile = load_json(path)
        profile_id = profile["id"]
        if profile_id in output:
            raise DesignProfileError(
                "DUPLICATE_PROFILE",
                f"duplicate profile id {profile_id!r} in {path}",
            )
        output[profile_id] = profile
    return output


def load_targets(directory: str | Path) -> dict[str, dict[str, Any]]:
    output = {}
    for path in sorted(Path(directory).glob("*.json")):
        target = load_json(path)
        target_id = target["id"]
        if target_id in output:
            raise DesignProfileError(
                "DUPLICATE_TARGET",
                f"duplicate target id {target_id!r} in {path}",
            )
        output[target_id] = target
    return output


def profile_diff(first: dict[str, Any], second: dict[str, Any]) -> dict[str, Any]:
    axes = {}
    for axis in STYLE_AXES:
        left = first["style_axes"].get(axis)
        right = second["style_axes"].get(axis)
        if left != right:
            axes[axis] = {"first": left, "second": right}
    return {
        "first": first["id"],
        "second": second["id"],
        "different_axes": axes,
        "count": len(axes),
    }


def select_profile(profiles_dir: str | Path, targets_dir: str | Path,
                   explicit_profile: str | None = None,
                   explicit_target: str | None = None,
                   active_path: str | Path | None = None,
                   mode: str = "design") -> dict[str, Any]:
    if mode not in {"design", "current"}:
        raise DesignProfileError("INVALID_SELECT_MODE", mode)
    profiles = load_profiles(profiles_dir)
    targets = load_targets(targets_dir)
    active: dict[str, Any] = {}
    if mode == "current" and active_path and Path(active_path).is_file():
        active = load_json(active_path)

    def choose(explicit: str | None, env_name: str,
               active_key: str, fallback: str) -> tuple[str, str]:
        if explicit:
            return explicit, "explicit"
        env_value = os.environ.get(env_name)
        if env_value:
            return env_value, "env"
        if active.get(active_key):
            return str(active[active_key]), "active"
        return fallback, "fallback"

    profile_id, profile_source = choose(
        explicit_profile, "DESIGN_PROFILE", "profile", "neutral")
    target_id, target_source = choose(
        explicit_target, "DESIGN_TARGET", "target", "generic_constrained")
    if profile_id not in profiles:
        raise DesignProfileError("UNKNOWN_PROFILE", profile_id)
    if target_id not in targets:
        raise DesignProfileError("UNKNOWN_TARGET", target_id)
    if profile_source == "active" and "profile_version" in active:
        if active["profile_version"] != profiles[profile_id]["version"]:
            raise DesignProfileError(
                "VERSION_MISMATCH",
                f"active profile version {active['profile_version']} != "
                f"{profiles[profile_id]['version']}",
            )
    if target_source == "active" and "target_version" in active:
        if active["target_version"] != targets[target_id]["version"]:
            raise DesignProfileError(
                "VERSION_MISMATCH",
                f"active target version {active['target_version']} != "
                f"{targets[target_id]['version']}",
            )
    result = resolve(profiles[profile_id], targets[target_id])
    result["selection"] = {
        "mode": mode,
        "profile_source": profile_source,
        "target_source": target_source,
    }
    return result


def check_current(profile_path: str | Path, colors_header: str | Path,
                  ui_header: str | Path) -> list[dict[str, str]]:
    profile = load_json(profile_path)
    compatibility = profile.get("compatibility")
    if not isinstance(compatibility, dict):
        return [{"code": "MISSING_COMPATIBILITY", "message": "profile has no compatibility map"}]
    color_text = Path(colors_header).read_text(encoding="utf-8")
    ui_text = Path(ui_header).read_text(encoding="utf-8")
    color_defines = {name: int(value, 16) for name, value in COLOR_DEFINE_RE.findall(color_text)}
    metric_defines = {name: int(value) for name, value in METRIC_DEFINE_RE.findall(ui_text)}
    alias_defines = {name: target for name, target in ALIAS_DEFINE_RE.findall(ui_text)}
    findings: list[dict[str, str]] = []
    for role, macro in compatibility.get("colors", {}).items():
        role_value = profile.get("roles", {}).get(role)
        if not isinstance(role_value, str) or not role_value.startswith("#"):
            findings.append({"code": "MISSING_ROLE", "message": f"{role} not in profile"})
            continue
        expected = int(role_value.lstrip("#"), 16)
        actual = color_defines.get(macro)
        if actual is None:
            findings.append({"code": "MISSING_MACRO", "message": f"{macro} not found"})
        elif actual != expected:
            findings.append({
                "code": "COLOR_DRIFT",
                "message": f"{role}: profile #{expected:06X} vs {macro} 0x{actual:06X}",
            })
    for role, alias_macro in compatibility.get("app_aliases", {}).items():
        target_macro = compatibility.get("colors", {}).get(role)
        actual_target = alias_defines.get(alias_macro)
        if actual_target is None:
            findings.append({
                "code": "MISSING_MACRO",
                "message": f"{alias_macro} not found",
            })
        elif target_macro is None:
            findings.append({
                "code": "MISSING_ROLE",
                "message": f"{role} has an app alias but no compatibility color",
            })
        elif actual_target != target_macro:
            findings.append({
                "code": "ALIAS_DRIFT",
                "message": f"{alias_macro} -> {actual_target}, expected {target_macro}",
            })
    for macro, expected in compatibility.get("metric_checks", {}).items():
        actual = metric_defines.get(macro)
        if actual is None:
            findings.append({"code": "MISSING_MACRO", "message": f"{macro} not found"})
        elif actual != expected:
            findings.append({
                "code": "METRIC_DRIFT",
                "message": f"{macro}: profile {expected} vs header {actual}",
            })
    return findings


def _default_dirs() -> tuple[Path, Path, Path]:
    root = repo_root()
    return root / "doc" / "design" / "profiles", root / "doc" / "design" / "targets", root / "doc" / "design" / "active.json"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    validate_parser = subparsers.add_parser("validate")
    validate_parser.add_argument("--profiles-dir", default=None)
    validate_parser.add_argument("--targets-dir", default=None)

    resolve_parser = subparsers.add_parser("resolve")
    resolve_parser.add_argument("--profile", required=True)
    resolve_parser.add_argument("--target", required=True)
    resolve_parser.add_argument("--profiles-dir", default=None)
    resolve_parser.add_argument("--targets-dir", default=None)
    resolve_parser.add_argument("--output", default=None)

    select_parser = subparsers.add_parser("select")
    select_parser.add_argument("--profile", default=None)
    select_parser.add_argument("--target", default=None)
    select_parser.add_argument("--profiles-dir", default=None)
    select_parser.add_argument("--targets-dir", default=None)
    select_parser.add_argument("--active", default=None)
    select_parser.add_argument("--mode", choices=("design", "current"), default="design")
    select_parser.add_argument("--output", default=None)

    diff_parser = subparsers.add_parser("diff")
    diff_parser.add_argument("first")
    diff_parser.add_argument("second")
    diff_parser.add_argument("--profiles-dir", default=None)

    current_parser = subparsers.add_parser("check-current")
    current_parser.add_argument("--profile", default=None)
    current_parser.add_argument("--colors-header", default=None)
    current_parser.add_argument("--ui-header", default=None)

    args = parser.parse_args(argv)
    profiles_dir, targets_dir, active_path = _default_dirs()
    if getattr(args, "profiles_dir", None):
        profiles_dir = Path(args.profiles_dir)
    if getattr(args, "targets_dir", None):
        targets_dir = Path(args.targets_dir)

    try:
        if args.command == "validate":
            profiles = load_profiles(profiles_dir)
            targets = load_targets(targets_dir)
            failures: list[str] = []
            for profile in profiles.values():
                failures.extend(validate_profile(profile))
            for target in targets.values():
                failures.extend(validate_target(target))
            pair_result = validate_pairs(profiles, targets)
            failures.extend(pair_result["failures"])
            if failures:
                print(json.dumps({"ok": False, "errors": failures,
                                  "pairs": pair_result["pairs"]}, indent=2))
                return 1
            print(json.dumps({"ok": True, "profiles": sorted(profiles),
                              "targets": sorted(targets),
                              "pairs": pair_result["pairs"]}, indent=2))
            return 0
        if args.command == "resolve":
            profiles = load_profiles(profiles_dir)
            targets = load_targets(targets_dir)
            if args.profile not in profiles:
                raise DesignProfileError("UNKNOWN_PROFILE", args.profile)
            if args.target not in targets:
                raise DesignProfileError("UNKNOWN_TARGET", args.target)
            result = resolve(profiles[args.profile], targets[args.target])
            text = json.dumps(result, indent=2)
            if args.output:
                Path(args.output).write_text(text + "\n", encoding="utf-8")
            print(text)
            return 0
        if args.command == "select":
            result = select_profile(
                profiles_dir, targets_dir, args.profile, args.target,
                Path(args.active) if args.active else active_path, args.mode)
            text = json.dumps(result, indent=2)
            if args.output:
                Path(args.output).write_text(text + "\n", encoding="utf-8")
            print(text)
            return 0
        if args.command == "diff":
            profiles = load_profiles(profiles_dir)
            for profile_id in (args.first, args.second):
                if profile_id not in profiles:
                    raise DesignProfileError("UNKNOWN_PROFILE", profile_id)
            result = profile_diff(profiles[args.first], profiles[args.second])
            print(json.dumps(result, indent=2))
            return 0 if result["count"] >= 3 else 1
        if args.command == "check-current":
            root = repo_root()
            profile_path = Path(args.profile) if args.profile else root / "doc" / "design" / "profiles" / "dark_sparse.json"
            colors_header = Path(args.colors_header) if args.colors_header else root / "layers" / "app_manager" / "app_theme" / "include" / "app_theme_colors.h"
            ui_header = Path(args.ui_header) if args.ui_header else root / "layers" / "apps" / "common" / "include" / "app_ui_theme.h"
            findings = check_current(profile_path, colors_header, ui_header)
            print(json.dumps({"ok": not findings, "findings": findings}, indent=2))
            return 0 if not findings else 1
    except (DesignProfileError, KeyError) as error:
        print(json.dumps({"ok": False, "error": str(error)}, indent=2))
        return 2
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
