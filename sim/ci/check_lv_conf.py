#!/usr/bin/env python3
"""Three-way drift check for the simulator LVGL profile.

Compares, for every CONFIG_LV_* macro:
  1. sim/lv_conf.h
  2. build/sim/gen_inc/sdkconfig.h (generated mirror of the device Kconfig)
  3. sdkconfig.defaults      (device source of truth)
and, when the device build tree exists, build/config/sdkconfig.h (expanded
Kconfig). Also enforces the hard gates the root CMakeLists.txt imposes on the
firmware (RGB565, libc allocator/string/sprintf, style cache) plus the
profile items the design doc calls out explicitly: LV_USE_CANVAS,
LV_USE_SNAPSHOT, LV_USE_IMAGE, LV_FREETYPE_USE_LVGL_PORT=1, LV_USE_FS_POSIX=0.

Run after CMake configure so the generated mirror exists:
  python3 sim/ci/check_lv_conf.py --mirror build/sim/gen_inc/sdkconfig.h
"""

import argparse
import os
import re
import sys

SIM_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MT_ROOT = os.path.dirname(SIM_ROOT)

DEFINE_RE = re.compile(r'^\s*#\s*define\s+([A-Za-z_]\w*)(?:\s+([^\s/].*?))?\s*(?:/\*\*?.*\*/)?\s*$',
                       re.M)


def parse_defines(path):
    if not os.path.exists(path):
        return None
    text = open(path, encoding='utf-8').read()
    return {m.group(1): m.group(2) for m in DEFINE_RE.finditer(text)}


def parse_defaults(path):
    values = {}
    for line in open(path, encoding='utf-8'):
        m = re.match(r'^(CONFIG_\w+)=([0-9]+|y|"[^"]*")$', line.strip())
        if m:
            raw = m.group(2)
            values[m.group(1)] = '1' if raw == 'y' else raw
    return values


def norm(value):
    if value is None:
        return None
    return value.strip()


def fail(errors, msg):
    errors.append(msg)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        '--mirror',
        default=os.path.join(MT_ROOT, 'build', 'sim', 'gen_inc', 'sdkconfig.h'),
        help='generated sdkconfig.h from sim CMake configure')
    args = parser.parse_args()
    errors = []
    lv_conf = parse_defines(os.path.join(SIM_ROOT, 'lv_conf.h'))
    mirror_path = args.mirror
    mirror = parse_defines(mirror_path)
    device = parse_defines(os.path.join(MT_ROOT, 'build/config/sdkconfig.h'))
    defaults = parse_defaults(os.path.join(MT_ROOT, 'sdkconfig.defaults'))
    if lv_conf is None or mirror is None:
        print('missing lv_conf.h or sim sdkconfig mirror', file=sys.stderr)
        return 2

    # Every CONFIG_LV_* in the mirror must have an agreeing LV_* in lv_conf.h.
    special = {
        'CONFIG_LV_OS_NONE': ('LV_USE_OS', 'LV_OS_NONE'),
        'CONFIG_LV_FONT_DEFAULT_MONTSERRAT_18': ('LV_FONT_DEFAULT', '&lv_font_montserrat_18'),
        'CONFIG_LV_CONF_SKIP': None,
        'CONFIG_LV_COLOR_DEPTH_16': None,
        'CONFIG_LV_DRAW_SW_ASM_NONE': None,
    }
    for name, mval in sorted(mirror.items()):
        if not name.startswith('CONFIG_LV_'):
            continue
        if name in special:
            entry = special[name]
            if entry is None:
                continue
            lv_name, lv_val = entry
            if norm(lv_conf.get(lv_name)) != lv_val:
                fail(errors, '%s: %s=%r but device %s=%s'
                     % (lv_conf_path, lv_name, lv_conf.get(lv_name), name, mval))
            continue
        if name.startswith('CONFIG_LV_BUILD_'):
            # Component build options (demos/examples): controlled by the
            # sim CMake cache (CONFIG_LV_BUILD_*_OFF), not lv_conf.h.
            continue
        if name in ('CONFIG_LV_USE_CLIB_MALLOC',
                    'CONFIG_LV_USE_CLIB_STRING',
                    'CONFIG_LV_USE_CLIB_SPRINTF'):
            stdlib = 'LV_USE_STDLIB_' + name.split('_')[-1]
            if norm(lv_conf.get(stdlib)) != 'LV_STDLIB_CLIB':
                fail(errors, '%s: %s must be LV_STDLIB_CLIB for %s'
                     % (lv_conf_path, stdlib, name))
            if norm(lv_conf.get(name[len('CONFIG_'):])) != norm(mval):
                fail(errors, '%s missing %s matching mirror %s'
                     % (lv_conf_path, name[len('CONFIG_'):], name))
            continue
        lv_name = name[len('CONFIG_'):]
        lv_val = norm(lv_conf.get(lv_name))
        if lv_val is None:
            fail(errors, 'lv_conf.h does not define %s (mirror has %s=%s)'
                 % (lv_name, name, mval))
            continue
        if lv_val != norm(mval):
            fail(errors, 'lv_conf.h %s=%s disagrees with sdkconfig mirror %s=%s'
                 % (lv_name, lv_val, name, mval))
        if device is not None:
            dval = norm(device.get(name))
            if dval is not None and dval != norm(mval):
                fail(errors, 'sdkconfig mirror %s=%s disagrees with device %s'
                     % (name, mval, dval))
        dflt = norm(defaults.get(name))
        if dflt is not None and dflt != norm(mval):
            fail(errors, 'sdkconfig mirror %s=%s disagrees with sdkconfig.defaults %s'
                 % (name, mval, dflt))

    # Hard gates from the root CMakeLists.txt / design doc.
    gates = {
        'LV_COLOR_DEPTH': '16',
        'LV_USE_CLIB_MALLOC': '1',
        'LV_USE_CLIB_STRING': '1',
        'LV_USE_CLIB_SPRINTF': '1',
        'LV_OBJ_STYLE_CACHE': '1',
        'LV_DEF_REFR_PERIOD': '15',
        'LV_DRAW_SW_DRAW_UNIT_CNT': '1',
        'LV_USE_CANVAS': '1',
        'LV_USE_GESTURE_RECOGNITION': '1',
        'LV_USE_FLOAT': '1',
        'LV_USE_QRCODE': '1',
        'LV_USE_FREETYPE': '1',
        'LV_FREETYPE_USE_LVGL_PORT': '1',
        'LV_USE_SNAPSHOT': '1',
        'LV_USE_IMAGE': '1',
        'LV_USE_FS_POSIX': '0',
    }
    for name, want in gates.items():
        got = norm(lv_conf.get(name))
        if got != want:
            fail(errors, 'gate %s=%r but lv_conf.h %s=%r'
                 % (name, want, name, got))
    if norm(lv_conf.get('LV_USE_OS')) != 'LV_OS_NONE':
        fail(errors, 'lv_conf.h LV_USE_OS must be LV_OS_NONE')
    if norm(lv_conf.get('LV_FONT_DEFAULT')) != '&lv_font_montserrat_18':
        fail(errors, 'lv_conf.h LV_FONT_DEFAULT must be &lv_font_montserrat_18')

    if not gates.get('LV_USE_FS_POSIX') == '0':
        fail(errors, 'internal gate error')

    if errors:
        print('check_lv_conf: FAIL (%d issue(s))' % len(errors), file=sys.stderr)
        for e in errors:
            print('  - %s' % e, file=sys.stderr)
        return 1
    scope = 'lv_conf/mirror/defaults'
    if device is not None:
        scope += '/device-autoconf'
    print('check_lv_conf: OK (%s)' % scope)
    return 0


lv_conf_path = os.path.join(SIM_ROOT, 'lv_conf.h')

if __name__ == '__main__':
    sys.exit(main())
