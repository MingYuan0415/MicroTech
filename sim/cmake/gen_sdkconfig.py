#!/usr/bin/env python3
"""Generate the simulator sdkconfig mirror from the device build autoconf.

Source of truth: build/config/sdkconfig.h - the exact CONFIG_* macro set the
firmware's C translation units see (including the private weather endpoint
and token from the local `sdkconfig`). `#define` lines are copied verbatim so
aliases/expressions keep working.

Freshness: symbols that layers/main/sim sources actually reference, plus
every CONFIG_LV_* (lv_conf.h mirrors that profile but no sim C source uses
the macros), are compared by VALUE between `sdkconfig` and the expansion.
sdkconfig keeps historical Kconfig remnants the expansion legitimately
drops, which is why a full-file or mtime-only check false-positives; any
in-scope disagreement means the Kconfig was edited without a rebuild -> fail.
"""
import json
import os
import re
import sys

DEFINE_RE = re.compile(
    r'^\s*#\s*define\s+(CONFIG_\w+)(?:\s+(.*?))?\s*(?:/\*.*\*/)?\s*$')
DOT_RE = re.compile(r'^(?:CONFIG_(\w+)=(.*)|# CONFIG_(\w+) is not set)$')
LIT_RE = re.compile(r'^(?:-?\d+|0[xX][0-9a-fA-F]+|"(?:[^"\\]|\\.)*")$')


def load_dotconfig(path):
    """Parse `sdkconfig` into {CONFIG_name: python value}."""
    values = {}
    with open(path, encoding='utf-8', errors='replace') as handle:
        for line in handle:
            match = DOT_RE.match(line.strip())
            if not match:
                continue
            if match.group(3) is not None:
                values['CONFIG_' + match.group(3)] = False
                continue
            name = 'CONFIG_' + match.group(1)
            raw = match.group(2)
            if raw == 'y':
                values[name] = True
            elif raw == 'n':
                values[name] = False
            elif raw.startswith('"') and raw.endswith('"') and len(raw) > 1:
                values[name] = raw[1:-1]
            else:
                try:
                    values[name] = int(raw, 0)
                except ValueError:
                    values[name] = raw
    return values


def normalized_define(value):
    """Map an autoconf literal to dotconfig space; None for aliases."""
    value = value.strip()
    if value == '1':
        return True
    if not LIT_RE.match(value):
        return None
    if value.startswith('"'):
        return json.loads(value)
    return int(value, 0)


def collect_usage(root):
    usage = set()
    pattern = re.compile(r'CONFIG_\w+')
    for base in ('layers', 'main', 'sim'):
        for dirpath, dirnames, filenames in os.walk(os.path.join(root, base)):
            dirnames[:] = [d for d in dirnames if d not in
                           ('tests', 'build', 'XPowersLib')]
            for fname in filenames:
                if fname.endswith(('.c', '.h')):
                    try:
                        text = open(os.path.join(dirpath, fname),
                                    encoding='utf-8',
                                    errors='replace').read()
                    except OSError:
                        continue
                    usage.update(pattern.findall(text))
    return usage


def check_fresh(root, defines):
    stale = []
    sdkconfig = os.path.join(root, 'sdkconfig')
    if not os.path.exists(sdkconfig):
        return stale
    dot = load_dotconfig(sdkconfig)
    usage = collect_usage(root)
    usage |= {name for name in dot if name.startswith('CONFIG_LV_')}
    for name in sorted(usage & set(dot)):
        want = dot[name]
        raw = defines.get(name)
        have = False if raw is None else normalized_define(raw)
        if have is None:
            continue
        if want != have:
            stale.append((name, want, have))
    return stale


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    root, source, output = sys.argv[1], sys.argv[2], sys.argv[3]

    if not os.path.exists(source):
        print('ERROR: %s not found. Run `idf.py build` once to expand the'
              ' project Kconfig (same prerequisite as managed_components).'
              % source, file=sys.stderr)
        return 1

    defines = {}
    lines = []
    with open(source, encoding='utf-8', errors='replace') as handle:
        for line in handle:
            match = DEFINE_RE.match(line)
            if match and match.group(2):
                if match.group(1) not in defines:
                    defines[match.group(1)] = match.group(2)
                    lines.append('#define %s %s'
                                 % (match.group(1), match.group(2).strip()))

    stale = check_fresh(root, defines)
    if stale:
        print('ERROR: in-scope CONFIG_ symbols differ between `sdkconfig`'
              ' and %s (%d item(s)); run `idf.py build` before building the'
              ' simulator.' % (os.path.basename(source), len(stale)),
              file=sys.stderr)
        for name, want, have in stale[:6]:
            print('  %s: sdkconfig=%r expansion=%r' % (name, want, have),
                  file=sys.stderr)
        return 1

    with open(output, 'w') as handle:
        handle.write('/* Generated by sim/cmake/gen_sdkconfig.py from %s.\n'
                     ' * DO NOT EDIT, DO NOT COMMIT (lives under build/).\n'
                     ' * Single source of truth: the device autoconf, so the\n'
                     ' * simulator compiles with the exact CONFIG_* set the\n'
                     ' * firmware sees (weather endpoint/token included).\n'
                     ' */\n'
                     '#ifndef SIM_GENERATED_SDKCONFIG_H\n'
                     '#define SIM_GENERATED_SDKCONFIG_H\n\n')
        for line in lines:
            handle.write(line + '\n')
        handle.write('\n#endif /* SIM_GENERATED_SDKCONFIG_H */\n')
    print('gen_sdkconfig: %d macros -> %s' % (len(lines), output))
    return 0


if __name__ == '__main__':
    sys.exit(main())
