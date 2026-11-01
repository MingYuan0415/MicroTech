#!/usr/bin/env python3
"""Check a rendered LVGL tree against a resolved design context.

This is a prompt generator, not a universal proof of visual quality. It checks
the mechanical parts a tree can prove: palette membership (background, text and
visible border colours), contrast, radius, spacing values, and touch-target
size. Touch checks include `styles.ext_click_pad` and clickable-row bounds when
the tree exposes them.
Composition, hierarchy and copy still need a screenshot and human/agent review.
Spacing is checked by default; `--no-spacing` skips computed padding when
`sim.tree` exposes theme defaults or intentional optical values that cannot be
compared with profile tokens.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

TOUCH_TYPES = {"lv_obj", "lv_button", "lv_switch", "lv_slider",
               "lv_checkbox", "lv_dropdown", "lv_roller"}
TEXT_TYPES = {"lv_label"}


def _hex(value: Any) -> str | None:
    if isinstance(value, str) and value.startswith("#") and len(value) == 7:
        return value.upper()
    return None


def _srgb(value: int) -> float:
    normalized = value / 255.0
    return normalized / 12.92 if normalized <= 0.04045 else ((normalized + 0.055) / 1.055) ** 2.4


def luminance(color: str) -> float:
    value = color.lstrip("#")
    red, green, blue = (int(value[index:index + 2], 16) for index in (0, 2, 4))
    return 0.2126 * _srgb(red) + 0.7152 * _srgb(green) + 0.0722 * _srgb(blue)


def contrast(first: str, second: str) -> float:
    a, b = luminance(first), luminance(second)
    lighter, darker = max(a, b), min(a, b)
    return (lighter + 0.05) / (darker + 0.05)


def _style(node: dict[str, Any]) -> dict[str, Any]:
    value = node.get("styles")
    return value if isinstance(value, dict) else {}


def _coords(node: dict[str, Any]) -> dict[str, Any]:
    value = node.get("coords")
    return value if isinstance(value, dict) else {}


def _expanded_rect(node: dict[str, Any]) -> dict[str, float]:
    coords = _coords(node)
    styles = _style(node)
    pad = styles.get("ext_click_pad", 0)
    if not isinstance(pad, (int, float)) or pad < 0:
        pad = 0
    try:
        return {
            "x": float(coords["x"]) - pad,
            "y": float(coords["y"]) - pad,
            "w": float(coords["w"]) + 2 * pad,
            "h": float(coords["h"]) + 2 * pad,
        }
    except (KeyError, TypeError, ValueError):
        return {}


def _contains(outer: dict[str, float], inner: dict[str, float]) -> bool:
    if not outer or not inner:
        return False
    cx = inner["x"] + inner["w"] / 2
    cy = inner["y"] + inner["h"] / 2
    return (outer["x"] <= cx <= outer["x"] + outer["w"] and
            outer["y"] <= cy <= outer["y"] + outer["h"])


def _node_label(node: dict[str, Any]) -> str:
    kind = node.get("type", "unknown")
    coords = _coords(node)
    return f"{kind}@{coords.get('x', '?')},{coords.get('y', '?')}"


def _covers(outer: dict[str, Any], inner: dict[str, Any]) -> bool:
    ox, oy = outer.get("x"), outer.get("y")
    ow, oh = outer.get("w"), outer.get("h")
    ix, iy = inner.get("x"), inner.get("y")
    iw, ih = inner.get("w"), inner.get("h")
    if not all(isinstance(value, (int, float)) for value in
               (ox, oy, ow, oh, ix, iy, iw, ih)):
        return False
    return ix <= ox and iy <= oy and ix + iw >= ox + ow and iy + ih >= oy + oh


def _is_hidden(node: dict[str, Any]) -> bool:
    flags = node.get("flags") or {}
    return bool(flags.get("hidden")) or flags.get("visible") is False


def _is_row_container(node: dict[str, Any], min_touch: int) -> bool:
    """Identify a clickable lv_obj row, not a scrim or full-page container."""
    if node.get("type") != "lv_obj" or min_touch <= 0:
        return False
    flags = node.get("flags") or {}
    if flags.get("scrollable") is True:
        return False
    height = _coords(node).get("h", 0)
    if not isinstance(height, (int, float)) or height <= 0:
        return False
    # Rows are one or two control-heights tall; overlays and page roots are not.
    return height <= max(72, min_touch + 24)


def _has_opaque_cover(node: dict[str, Any]) -> bool:
    if _is_hidden(node):
        return False
    coords = _coords(node)
    for child in node.get("children") or []:
        if not isinstance(child, dict):
            continue
        if _is_hidden(child):
            continue
        styles = _style(child)
        if (isinstance(styles.get("bg_opa"), (int, float)) and
                styles["bg_opa"] >= 200 and _covers(coords, _coords(child))):
            return True
        if _has_opaque_cover(child):
            return True
    return False


def _finding(code: str, severity: str, node: dict[str, Any], message: str) -> dict[str, Any]:
    return {
        "code": code,
        "severity": severity,
        "node": _node_label(node),
        "message": message,
    }


def audit_tree(tree: dict[str, Any], context: dict[str, Any],
               check_spacing: bool = True) -> dict[str, Any]:
    resolved = context["resolved"]
    rules = context["profile"]["rules"]
    style_axes = context["profile"]["style_axes"]
    colors = {value.upper() for value in resolved["colors"].values()}
    role_map: dict[str, set[str]] = {}
    for role, value in resolved["colors"].items():
        role_map.setdefault(value.upper(), set()).add(role)
    canvas = resolved["colors"]["canvas"].upper()
    radius_scale = {int(value) for value in resolved["radius_px"]}
    space_scale = {int(value) for value in resolved["space_px"]}
    min_touch = int(resolved["min_touch_px"])
    max_icons = int(resolved["max_icon_count"])
    max_accents = int(rules.get("max_accent_roles", 99))
    image_ids: set[str] = set()
    unknown_image_nodes: list[str] = []
    base_roles = {"canvas", "surface_1", "surface_2", "surface_pressed",
                  "surface_low", "border", "track", "ink", "ink_muted",
                  "ink_faint", "on_accent"}
    accent_roles = {"accent", "success", "warning", "danger",
                    "accent_tint", "success_tint", "warning_tint", "danger_tint"}
    accent_colors = {
        color for color, roles in role_map.items()
        if roles & accent_roles and not roles & base_roles
    }
    mono_target = context["target"]["display"]["color_mode"] == "mono"
    accents_used: set[str] = set()
    min_contrast_body = float(resolved.get(
        "min_contrast_body", rules.get("min_contrast_body", 4.5)))
    min_contrast_large = float(resolved.get(
        "min_contrast_large", rules.get("min_contrast_large", 3.0)))
    min_contrast_faint = float(resolved.get(
        "min_contrast_faint", rules.get("min_contrast_faint", 4.0)))
    findings: list[dict[str, Any]] = []

    tree_profile = tree.get("profile_id")
    tree_target = tree.get("target_id")
    if tree_profile and tree_profile != context["profile"]["id"]:
        findings.append({
            "code": "PROFILE_MISMATCH",
            "severity": "blocker",
            "node": "tree",
            "message": f"tree is {tree_profile}, context is {context['profile']['id']}",
        })
    if tree_target and tree_target != context["target"]["id"]:
        findings.append({
            "code": "TARGET_MISMATCH",
            "severity": "blocker",
            "node": "tree",
            "message": f"tree is {tree_target}, context is {context['target']['id']}",
        })

    def visit(node: dict[str, Any], inherited_bg: str, hidden: bool = False,
              clickable_ancestors: tuple[dict[str, Any], ...] = ()) -> None:
        if not isinstance(node, dict):
            return
        flags = node.get("flags") or {}
        if hidden or _is_hidden(node):
            return
        styles = _style(node)
        bg_color = _hex(styles.get("bg_color"))
        bg_opa = styles.get("bg_opa", 0)
        bg_visible = isinstance(bg_opa, (int, float)) and bg_opa >= 200
        border_color = _hex(styles.get("border_color"))
        border_width = styles.get("border_width", 0)
        border_opa = styles.get("border_opa", 255)
        border_visible = (
            isinstance(border_width, (int, float)) and border_width > 0 and
            isinstance(border_opa, (int, float)) and border_opa >= 200)
        opaque_bg = bg_color if bg_visible else inherited_bg
        text_color = _hex(styles.get("text_color"))
        text = node.get("text")
        node_type = node.get("type", "")
        image_id = node.get("image_semantic_id")
        if isinstance(image_id, str) and image_id:
            if image_id == "unknown":
                unknown_image_nodes.append(_node_label(node))
            else:
                image_ids.add(image_id)

        target_rect = _expanded_rect(node) or _coords(node)
        if node_type != "lv_button":
            for ancestor in reversed(clickable_ancestors):
                ancestor_type = ancestor.get("type", "")
                if ancestor_type not in TOUCH_TYPES:
                    continue
                if ancestor_type == "lv_obj" and not _is_row_container(ancestor, min_touch):
                    continue
                ancestor_rect = _expanded_rect(ancestor) or _coords(ancestor)
                if _contains(ancestor_rect, target_rect):
                    target_rect = ancestor_rect
                    break

        if opaque_bg and not mono_target and opaque_bg in accent_colors:
            accents_used.add(opaque_bg)
        if opaque_bg and opaque_bg not in colors and not _has_opaque_cover(node):
            findings.append(_finding(
                "BG_COLOR_NOT_IN_PROFILE", "major", node,
                f"opaque background {opaque_bg} is not a profile role"))
        if border_visible and border_color and border_color not in colors:
            findings.append(_finding(
                "BORDER_COLOR_NOT_IN_PROFILE", "major", node,
                f"visible border colour {border_color} is not a profile role"))
        if text_color and (text is not None or node_type in TEXT_TYPES):
            if text_color not in colors:
                findings.append(_finding(
                    "TEXT_COLOR_NOT_IN_PROFILE", "major", node,
                    f"text colour {text_color} is not a profile role"))
            if opaque_bg:
                line_height = styles.get("line_height", 0)
                roles = role_map.get(text_color, set())
                if "ink_faint" in roles:
                    threshold = min_contrast_faint
                elif isinstance(line_height, (int, float)) and line_height >= 32:
                    threshold = min_contrast_large
                else:
                    threshold = min_contrast_body
                ratio = contrast(text_color, opaque_bg)
                if ratio + 0.01 < threshold:
                    findings.append(_finding(
                        "CONTRAST_FAIL", "blocker", node,
                        f"{ratio:.2f}:1 between {text_color} and {opaque_bg}, "
                        f"needs {threshold:.2f}:1"))

        radius = styles.get("radius")
        radius_visible = bg_visible or border_visible
        if (radius_visible and isinstance(radius, (int, float)) and radius > 0 and
                int(radius) not in radius_scale and int(radius) != 32767):
            findings.append(_finding(
                "OFF_SCALE_RADIUS", "minor", node,
                f"radius {int(radius)} is not in {sorted(radius_scale)}"))

        if check_spacing:
            for key in ("pad_left", "pad_right", "pad_top", "pad_bottom",
                        "pad_row", "pad_column"):
                value = styles.get(key)
                if isinstance(value, (int, float)) and value > 0 and int(value) not in space_scale:
                    findings.append(_finding(
                        "OFF_SCALE_SPACING", "minor", node,
                        f"{key}={int(value)} is not in {sorted(space_scale)}"))
                    break

        width = target_rect.get("w", 0)
        height = target_rect.get("h", 0)
        clickable = flags.get("clickable") is True
        touch_target = node_type in TOUCH_TYPES
        if node_type == "lv_obj" and flags.get("scrollable") is True:
            touch_target = False
        if (clickable and touch_target and min_touch > 0 and
                isinstance(width, (int, float)) and isinstance(height, (int, float)) and
                (width < min_touch or height < min_touch)):
            findings.append(_finding(
                "TOUCH_TARGET_SMALL", "blocker", node,
                f"{int(width)}x{int(height)} is below {min_touch}px minimum"))

        child_ancestors = clickable_ancestors
        if flags.get("clickable"):
            child_ancestors = clickable_ancestors + (node,)
        for child in node.get("children") or []:
            if isinstance(child, dict):
                visit(child, opaque_bg or canvas, False, child_ancestors)

    root = tree.get("tree", tree)
    if not isinstance(root, dict):
        raise ValueError("tree must be an object or contain a tree object")
    roots = [root]
    overlays = tree.get("overlays")
    if overlays is not None:
        if not isinstance(overlays, list):
            raise ValueError("tree.overlays must be a list")
        roots.extend(item for item in overlays if isinstance(item, dict))
    for root_node in roots:
        visit(root_node, canvas)
    if unknown_image_nodes:
        findings.append({
            "code": "UNKNOWN_IMAGE",
            "severity": "major",
            "node": "tree",
            "message": f"{len(unknown_image_nodes)} image node(s) have unknown semantic ids: {unknown_image_nodes[:5]}",
        })
    if len(image_ids) > max_icons:
        findings.append({
            "code": "TOO_MANY_ICONS",
            "severity": "major",
            "node": "tree",
            "message": f"{len(image_ids)} distinct image ids > {max_icons} budget",
        })
    if not mono_target and len(accents_used) > max_accents:
        findings.append({
            "code": "TOO_MANY_ACCENT_ROLES",
            "severity": "major",
            "node": "tree",
            "message": f"{len(accents_used)} accent colours > {max_accents} budget: {sorted(accents_used)}",
        })

    return {
        "profile_id": context["profile"]["id"],
        "target_id": context["target"]["id"],
        "style_axes": style_axes,
        "findings": findings,
        "summary": {
            "blockers": sum(1 for item in findings if item["severity"] == "blocker"),
            "major": sum(1 for item in findings if item["severity"] == "major"),
            "minor": sum(1 for item in findings if item["severity"] == "minor"),
        },
        "limitations": [
            "Tree checks cannot prove composition, hierarchy or copy quality.",
            "Spacing is checked by default; --no-spacing skips computed padding when theme defaults or optical exceptions apply.",
            "Hidden or off-screen subtrees are skipped; computed props can still hide optical states.",
            "Composite controls use extended click padding and clickable-row bounds when available; clickable lv_obj containers are treated as row targets only when their geometry is row-like.",
            "Touch/clickable checks require flags.clickable; passive nodes are not touch targets.",
            "Icon budget counts known image semantic ids only; unknown images are reported separately.",
            "Icon/border contrast (min_contrast_ui) and asset byte provenance are outside tree checks.",
            "Overlay roots are audited when the tree wrapper includes `overlays`; raw sim.tree contains only the active page.",
            "Raw sim.tree has no profile/target metadata; pass a wrapper to check mismatch.",
        ],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", required=True)
    parser.add_argument("--context", required=True)
    parser.add_argument("--no-spacing", action="store_true",
                        help="skip computed padding values")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    tree = json.loads(Path(args.tree).read_text(encoding="utf-8"))
    context = json.loads(Path(args.context).read_text(encoding="utf-8"))
    result = audit_tree(tree, context, check_spacing=not args.no_spacing)
    print(json.dumps(result, indent=2))
    if args.check and result["findings"]:
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
