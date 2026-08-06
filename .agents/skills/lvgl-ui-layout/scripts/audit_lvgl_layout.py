#!/usr/bin/env python3
"""Report suspicious LVGL layout patterns without modifying source files."""

from __future__ import annotations

import argparse
import re
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable


SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hh", ".hpp"}
EXCLUDED_PARTS = {
    ".git",
    ".idea",
    ".vscode",
    "build",
    "managed_components",
    "node_modules",
}

ABSOLUTE_POSITION_RE = re.compile(r"\blv_obj_set_(?:pos|x|y)\s*\(")
DOT_MODE_RE = re.compile(r"\bLV_LABEL_(?:LONG_)?(?:MODE_)?DOTS?\b")
LABEL_CREATE_RE = re.compile(
    r"(?P<var>[A-Za-z_]\w*(?:(?:->|\.)[A-Za-z_]\w*)*)\s*=\s*"
    r"lv_label_create\s*\("
)
BUTTON_CREATE_RE = re.compile(
    r"(?P<var>[A-Za-z_]\w*(?:(?:->|\.)[A-Za-z_]\w*)*)\s*=\s*"
    r"lv_(?:button|btn)_create\s*\("
)
OBJ_CREATE_RE = re.compile(
    r"(?P<var>[A-Za-z_]\w*(?:(?:->|\.)[A-Za-z_]\w*)*)\s*=\s*"
    r"lv_obj_create\s*\(\s*"
    r"(?P<parent>[A-Za-z_]\w*(?:(?:->|\.)[A-Za-z_]\w*)*)\s*\)"
)
WIDGET_CREATE_RE = re.compile(
    r"(?P<var>[A-Za-z_]\w*(?:(?:->|\.)[A-Za-z_]\w*)*)\s*=\s*"
    r"lv_[A-Za-z0-9_]+_create(?:_obj)?\s*\("
)
LABEL_TEXT_RE = re.compile(
    r"\blv_label_set_text(?:_fmt|_static)?\s*\(\s*"
    r"(?P<var>[A-Za-z_]\w*(?:(?:->|\.)[A-Za-z_]\w*)*)\s*,"
)
CLICKABLE_RE = re.compile(
    r"\blv_obj_add_flag\s*\(\s*"
    r"(?P<var>[A-Za-z_]\w*(?:(?:->|\.)[A-Za-z_]\w*)*)\s*,"
    r"[^;]*\bLV_OBJ_FLAG_CLICKABLE\b"
)
PASSIVE_INPUT_RE = re.compile(
    r"\blv_obj_remove_flag\s*\(\s*"
    r"(?P<var>[A-Za-z_]\w*(?:(?:->|\.)[A-Za-z_]\w*)*)\s*,"
    r"[^;]*\bLV_OBJ_FLAG_CLICKABLE\b"
)
EVENT_BUBBLE_RE = re.compile(
    r"\blv_obj_add_flag\s*\(\s*"
    r"(?P<var>[A-Za-z_]\w*(?:(?:->|\.)[A-Za-z_]\w*)*)\s*,"
    r"[^;]*\bLV_OBJ_FLAG_EVENT_BUBBLE\b"
)
EVENT_OWNER_RE = re.compile(
    r"\blv_obj_add_event_cb\s*\(\s*"
    r"(?P<var>[A-Za-z_]\w*(?:(?:->|\.)[A-Za-z_]\w*)*)\s*,"
    r"\s*[^,]+,\s*(?P<event>[^,]+),"
)
INPUT_EVENT_RE = re.compile(
    r"\bLV_EVENT_(?:ALL|PRESSED|PRESSING|PRESS_LOST|SHORT_CLICKED|"
    r"SINGLE_CLICKED|CLICKED|LONG_PRESSED|LONG_PRESSED_REPEAT|RELEASED|"
    r"GESTURE)\b"
)
FIXED_GEOMETRY_RE = re.compile(
    r"\blv_obj_set_(?:pos|x|y|width|height|size)\s*\([^;]*?"
    r"(?:,\s*-?\d+\s*){1,2}\)"
)
SYMBOL_HINT_RE = re.compile(
    r"(?:^|_)(?:symbol|icon|glyph|chevron)(?:_|\b)",
    re.IGNORECASE,
)
LOCAL_OBJ_DECL_RE = re.compile(
    r"\blv_obj_t\s*\*+\s*"
    r"(?P<var>[A-Za-z_]\w*)\s*="
)
TEXT_LITERAL_RE = re.compile(r'^\s*(?:u8|u|U|L)?"')

SETUP_PATTERNS = {
    "font": re.compile(r"\blv_obj_set_style_text_font\s*\("),
    "color": re.compile(r"\blv_obj_set_style_text_color\s*\("),
    "geometry": re.compile(
        r"\blv_obj_set_(?:width|height|size|flex_grow|grid_cell)\s*\("
    ),
    "long mode": re.compile(r"\blv_label_set_long_mode\s*\("),
}


@dataclass(frozen=True)
class Finding:
    path: Path
    line: int
    code: str
    message: str


@dataclass(frozen=True)
class Statement:
    code: str
    raw: str
    line: int
    scope: tuple[int, ...]


@dataclass(frozen=True)
class ObjectBinding:
    binding_id: int
    kind: str
    scope: tuple[int, ...]
    var: str
    line: int


@dataclass
class LabelAudit:
    binding: ObjectBinding
    first_text: Statement | None = None
    setup_before: dict[str, list[Statement]] = field(
        default_factory=lambda: defaultdict(list)
    )
    setup_after: dict[str, list[Statement]] = field(
        default_factory=lambda: defaultdict(list)
    )


def _is_excluded(path: Path) -> bool:
    return any(part in EXCLUDED_PARTS for part in path.parts)


def _source_files(inputs: Iterable[Path]) -> list[Path]:
    files: set[Path] = set()
    for item in inputs:
        if item.is_file() and item.suffix.lower() in SOURCE_SUFFIXES:
            files.add(item)
        elif item.is_dir():
            for candidate in item.rglob("*"):
                if (candidate.is_file()
                        and candidate.suffix.lower() in SOURCE_SUFFIXES
                        and not _is_excluded(candidate)):
                    files.add(candidate)
    return sorted(files)


def _sanitize_c(source: str) -> str:
    """Remove comments and literal contents while preserving offsets/newlines."""
    output = list(source)
    state = "code"
    escaped = False
    index = 0

    while index < len(source):
        char = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""

        if state == "code":
            if char == "/" and following == "/":
                output[index] = output[index + 1] = " "
                state = "line_comment"
                index += 2
                continue
            if char == "/" and following == "*":
                output[index] = output[index + 1] = " "
                state = "block_comment"
                index += 2
                continue
            if char == '"':
                output[index] = " "
                state = "string"
                escaped = False
            elif char == "'":
                output[index] = " "
                state = "char"
                escaped = False
        elif state == "line_comment":
            if char == "\n":
                state = "code"
            else:
                output[index] = " "
        elif state == "block_comment":
            if char == "*" and following == "/":
                output[index] = output[index + 1] = " "
                state = "code"
                index += 2
                continue
            if char != "\n":
                output[index] = " "
        else:
            if char != "\n":
                output[index] = " "
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif ((state == "string" and char == '"')
                  or (state == "char" and char == "'")):
                state = "code"

        index += 1

    return "".join(output)


def _strip_preprocessor(code: str) -> str:
    """Blank preprocessor directives and continuations while keeping lines."""
    output: list[str] = []
    continuation = False
    for line in code.splitlines(keepends=True):
        directive = continuation or line.lstrip().startswith("#")
        if directive:
            body = line.rstrip("\r\n")
            continuation = body.rstrip().endswith("\\")
            output.append("".join(
                char if char in "\r\n" else " " for char in line
            ))
        else:
            continuation = False
            output.append(line)
    return "".join(output)


def _logical_statements(source: str) -> list[Statement]:
    sanitized = _strip_preprocessor(_sanitize_c(source))
    statements: list[Statement] = []
    scope_stack: list[int] = []
    next_scope = 1
    start: int | None = None
    start_line = 1
    statement_scope: tuple[int, ...] = ()
    line = 1

    for index, char in enumerate(sanitized):
        if char == "{":
            scope_stack.append(next_scope)
            next_scope += 1
            start = None
        elif char == "}":
            start = None
            if scope_stack:
                scope_stack.pop()
        elif char == ";":
            if start is not None:
                code = sanitized[start:index + 1]
                raw = source[start:index + 1]
                statements.append(Statement(
                    code=re.sub(r"\s+", " ", code).strip(),
                    raw=raw,
                    line=start_line,
                    scope=statement_scope,
                ))
            start = None
        elif start is None and not char.isspace():
            start = index
            start_line = line
            statement_scope = tuple(scope_stack)

        if char == "\n":
            line += 1

    return statements


def _scope_chain(scope: tuple[int, ...]) -> Iterable[tuple[int, ...]]:
    for length in range(len(scope), -1, -1):
        yield scope[:length]


def _lookup_binding(
        bindings: dict[tuple[int, ...], dict[str, ObjectBinding]],
        scope: tuple[int, ...], var: str) -> ObjectBinding | None:
    if "->" in var or "." in var:
        root = scope[:1]
        return bindings.get(root, {}).get(var)
    for candidate_scope in _scope_chain(scope):
        binding = bindings.get(candidate_scope, {}).get(var)
        if binding is not None:
            return binding
    return None


def _binding_scope(
        bindings: dict[tuple[int, ...], dict[str, ObjectBinding]],
        statement: Statement, var: str) -> tuple[int, ...]:
    declaration = LOCAL_OBJ_DECL_RE.search(statement.code)
    if declaration and declaration.group("var") == var:
        return statement.scope
    if "->" in var or "." in var:
        return statement.scope[:1]
    existing = _lookup_binding(bindings, statement.scope, var)
    return existing.scope if existing is not None else statement.scope


def _call_var(statement: Statement,
              pattern: re.Pattern[str]) -> str | None:
    match = pattern.search(statement.code)
    if match is None:
        return None
    target = re.match(
        r"\s*(?P<var>[A-Za-z_]\w*(?:(?:->|\.)[A-Za-z_]\w*)*)\s*,",
        statement.code[match.end():],
    )
    return target.group("var") if target is not None else None


def _widget_kind(statement: Statement) -> str:
    if LABEL_CREATE_RE.search(statement.code):
        return "label"
    if BUTTON_CREATE_RE.search(statement.code):
        return "button"
    if OBJ_CREATE_RE.search(statement.code):
        return "generic"
    return "widget"


def _font_findings(path: Path, audit: LabelAudit) -> list[Finding]:
    text = audit.first_text
    if text is None:
        return []

    findings: list[Finding] = []
    var = audit.binding.var
    late_parts = [
        name for name in SETUP_PATTERNS
        if audit.setup_after[name] and not audit.setup_before[name]
    ]
    fonts = audit.setup_before["font"] + audit.setup_after["font"]
    if not fonts:
        findings.append(Finding(
            path,
            text.line,
            "MISSING_EXPLICIT_FONT",
            f"{var} receives initial text without an explicit font binding",
        ))
    else:
        text_match = LABEL_TEXT_RE.search(text.code)
        raw_match = LABEL_TEXT_RE.search(text.raw)
        value_code = text.code[text_match.end():] if text_match else ""
        raw_value = text.raw[raw_match.end():] if raw_match else ""
        direct_symbol = "LV_SYMBOL_" in value_code
        indirect_symbol = SYMBOL_HINT_RE.search(value_code) is not None
        direct_text = TEXT_LITERAL_RE.search(raw_value) is not None
        font_code = " ".join(item.code for item in fonts)

        if direct_symbol and "LV_FONT_DEFAULT" not in font_code:
            findings.append(Finding(
                path,
                text.line,
                "SYMBOL_FONT",
                f"{var} renders LV_SYMBOL_* without LV_FONT_DEFAULT",
            ))
        elif indirect_symbol:
            findings.append(Finding(
                path,
                text.line,
                "FONT_ROLE_REVIEW",
                f"{var} symbol-like text expression is indirect; "
                "trace it to LV_SYMBOL_* and LV_FONT_DEFAULT",
            ))
        elif direct_text and "LV_FONT_DEFAULT" in font_code:
            findings.append(Finding(
                path,
                text.line,
                "TEXT_FONT_ROLE",
                f"{var} renders ordinary text with LV_FONT_DEFAULT; "
                "bind an APP_THEME_FONT_* role",
            ))
        elif (not direct_text and "LV_FONT_DEFAULT" in font_code):
            findings.append(Finding(
                path,
                text.line,
                "FONT_ROLE_REVIEW",
                f"{var} text expression is indirect; verify whether it requires "
                "APP_THEME_FONT_* or LV_FONT_DEFAULT",
            ))
        elif "APP_THEME_FONT_" not in font_code:
            findings.append(Finding(
                path,
                text.line,
                "FONT_ROLE_REVIEW",
                f"{var} font role is indirect; trace it to APP_THEME_FONT_*",
            ))

    if late_parts:
        findings.append(Finding(
            path,
            text.line,
            "TEXT_BEFORE_SETUP",
            f"{var} receives initial text before "
            f"{', '.join(late_parts)} setup",
        ))
    return findings


def _audit_text(path: Path, source: str,
                fixed_clusters: bool) -> list[Finding]:
    statements = _logical_statements(source)
    findings: list[Finding] = []
    bindings: dict[tuple[int, ...], dict[str, ObjectBinding]] = defaultdict(dict)
    label_audits: dict[int, LabelAudit] = {}
    pending_button_children: dict[int, Finding] = {}
    fixed_lines: dict[tuple[int, ...], list[int]] = defaultdict(list)
    next_binding_id = 1

    for statement in statements:
        if ABSOLUTE_POSITION_RE.search(statement.code):
            findings.append(Finding(
                path,
                statement.line,
                "ABSOLUTE_POSITION",
                "review whether alignment, Flex, or Grid can own this position",
            ))
        if DOT_MODE_RE.search(statement.code):
            findings.append(Finding(
                path,
                statement.line,
                "DOT_TRUNCATION",
                "required text must wrap, scroll, or be made fully accessible",
            ))
        if fixed_clusters and FIXED_GEOMETRY_RE.search(statement.code):
            fixed_lines[statement.scope[:1]].append(statement.line)

        widget_create = WIDGET_CREATE_RE.search(statement.code)
        if widget_create:
            var = widget_create.group("var")
            obj_create = OBJ_CREATE_RE.search(statement.code)
            parent_binding = None
            if obj_create:
                parent_binding = _lookup_binding(
                    bindings, statement.scope, obj_create.group("parent")
                )

            target_scope = _binding_scope(bindings, statement, var)
            old_binding = bindings[target_scope].get(var)
            if old_binding is not None:
                pending = pending_button_children.pop(
                    old_binding.binding_id, None
                )
                if pending is not None:
                    findings.append(pending)

            binding = ObjectBinding(
                binding_id=next_binding_id,
                kind=_widget_kind(statement),
                scope=target_scope,
                var=var,
                line=statement.line,
            )
            next_binding_id += 1
            bindings[target_scope][var] = binding
            if binding.kind == "label":
                label_audits[binding.binding_id] = LabelAudit(binding)
            if (binding.kind == "generic" and parent_binding is not None
                    and parent_binding.kind == "button"):
                pending_button_children[binding.binding_id] = Finding(
                    path,
                    statement.line,
                    "BUTTON_CHILD_INPUT",
                    f"generic child {var} of button {parent_binding.var} keeps "
                    "the clickable default without explicit input ownership",
                )

        for name, pattern in SETUP_PATTERNS.items():
            var = _call_var(statement, pattern)
            if var is None:
                continue
            binding = _lookup_binding(bindings, statement.scope, var)
            if binding is None or binding.kind != "label":
                continue
            audit = label_audits[binding.binding_id]
            destination = (
                audit.setup_before if audit.first_text is None
                else audit.setup_after
            )
            destination[name].append(statement)

        text_call = LABEL_TEXT_RE.search(statement.code)
        if text_call:
            binding = _lookup_binding(
                bindings, statement.scope, text_call.group("var")
            )
            if binding is not None and binding.kind == "label":
                audit = label_audits[binding.binding_id]
                if audit.first_text is None:
                    audit.first_text = statement

        for pattern in (PASSIVE_INPUT_RE, EVENT_BUBBLE_RE):
            match = pattern.search(statement.code)
            if match:
                binding = _lookup_binding(
                    bindings, statement.scope, match.group("var")
                )
                if binding is not None:
                    pending_button_children.pop(binding.binding_id, None)

        event_owner = EVENT_OWNER_RE.search(statement.code)
        if (event_owner
                and INPUT_EVENT_RE.search(event_owner.group("event"))):
            binding = _lookup_binding(
                bindings, statement.scope, event_owner.group("var")
            )
            if binding is not None:
                pending_button_children.pop(binding.binding_id, None)

        clickable = CLICKABLE_RE.search(statement.code)
        if clickable:
            binding = _lookup_binding(
                bindings, statement.scope, clickable.group("var")
            )
            if binding is not None and binding.kind == "label":
                findings.append(Finding(
                    path,
                    statement.line,
                    "CLICKABLE_LABEL",
                    f"{binding.var} is a label; verify explicit input ownership",
                ))

    findings.extend(pending_button_children.values())
    for audit in label_audits.values():
        findings.extend(_font_findings(path, audit))

    if fixed_clusters:
        for function_lines in fixed_lines.values():
            remaining = function_lines
            while remaining:
                start = remaining[0]
                cluster = [line for line in remaining if line <= start + 24]
                if len(cluster) >= 4:
                    findings.append(Finding(
                        path,
                        start,
                        "FIXED_GEOMETRY_CLUSTER",
                        f"{len(cluster)} fixed geometry calls within 25 lines; "
                        "verify each invariant",
                    ))
                remaining = [line for line in remaining if line > start + 24]

    return findings


def _audit_file(path: Path, fixed_clusters: bool) -> list[Finding]:
    source = path.read_text(encoding="utf-8")
    return _audit_text(path, source, fixed_clusters)


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Audit C/C++ sources for risky LVGL layout patterns."
    )
    parser.add_argument(
        "paths",
        nargs="*",
        type=Path,
        default=[Path.cwd()],
        help="files or directories to scan (default: current directory)",
    )
    parser.add_argument(
        "--fixed-clusters",
        action="store_true",
        help="also report dense clusters of literal fixed geometry",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="return exit status 1 when findings are present",
    )
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    missing = [path for path in args.paths if not path.exists()]
    if missing:
        for path in missing:
            print(f"error: path does not exist: {path}", file=sys.stderr)
        return 2

    files = _source_files(args.paths)
    if not files:
        print("error: no C/C++ source files found", file=sys.stderr)
        return 2

    findings: list[Finding] = []
    decode_failed = False
    for path in files:
        try:
            findings.extend(_audit_file(path, args.fixed_clusters))
        except UnicodeDecodeError as error:
            print(f"error: cannot decode {path}: {error}", file=sys.stderr)
            decode_failed = True

    findings.sort(key=lambda item: (str(item.path), item.line, item.code))
    for finding in findings:
        print(
            f"{finding.path}:{finding.line}: "
            f"{finding.code}: {finding.message}"
        )

    counts = Counter(finding.code for finding in findings)
    summary = ", ".join(
        f"{code}={count}" for code, count in sorted(counts.items())
    )
    print(
        f"Scanned {len(files)} source files; found {len(findings)} review item(s)"
        + (f" ({summary})" if summary else ".")
    )
    if decode_failed:
        return 2
    return 1 if args.strict and findings else 0


if __name__ == "__main__":
    raise SystemExit(main())
