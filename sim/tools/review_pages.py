#!/usr/bin/env python3
"""Adversarial UI review harness for the MicroTech LVGL simulator.

Drives the sim agent through a per-page state matrix, dumps the widget tree,
runs geometry/state lint, and compares a masked PNG against a golden baseline.
This is the gate the `ui-review` skill describes: it catches wrapped text,
off-screen/unreachable controls, washed-out disabled states, and fallback icons
that structure-only assertions and single happy-path screenshots miss.

Spec schema (JSON):
  {
    "viewport": {"w": 368, "h": 448},          # optional; else read from tree
    "pages": [
      {"app": "settings", "page": "wifi",
       "states": [
         {"name": "idle_no_profile",
          "seed": [ {"cmd": "set_power", ...}, ... ],   # run_scenarios cmds
          "expect_visible": ["Wi-Fi 开关", "忘记网络"],
          "single_line":    ["IP 地址", "状态"],
          "allow_disabled": ["忘记网络"],
          "allow_disabled_types": ["lv_switch"],   # unlabeled intentional
          "expect_image":   [],
          "masks": [[x, y, w, h], ...],                  # volatile regions
          "hardware_only": false}
       ]}
    ]
  }

Usage:
  python3 sim/tools/review_pages.py --spec sim/ci/review/spec.json [--check]
      [--update] [--pages settings/wifi,...] [--golden DIR] [--shots DIR]
      [--build build/sim] [--port 5002] [--no-launch] [--keep-sim]
      [--report out.json]

Exit non-zero on any lint violation or baseline mismatch.
"""
import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import time

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
MT_ROOT = os.path.dirname(os.path.dirname(TOOLS_DIR))
sys.path.insert(0, TOOLS_DIR)
from run_scenarios import Runner  # noqa: E402

INTERACTIVE = {'lv_button', 'lv_switch', 'lv_slider', 'lv_roller',
               'lv_checkbox', 'lv_dropdown'}


def _port_open(host, port):
    try:
        socket.create_connection((host, port), timeout=1).close()
        return True
    except OSError:
        return False


def _launch_sim(build, port, nvs, shots, sd):
    shutil.rmtree(nvs, ignore_errors=True)
    shutil.rmtree(sd, ignore_errors=True)
    os.makedirs(shots, exist_ok=True)
    log = open(os.path.join(shots, 'review_sim.log'), 'wb')
    proc = subprocess.Popen(
        [os.path.join(build, 'microtech_sim'), '--headless', '--ci',
         '--res-dir', os.path.join(build, 'sim_res_fs'),
         '--nvs-dir', nvs, '--sd-dir', sd, '--out-dir', shots,
         '--agent-port', str(port)], stdout=log, stderr=subprocess.STDOUT)
    for _ in range(60):
        if _port_open('127.0.0.1', port):
            return proc
        if proc.poll() is not None:
            raise RuntimeError('sim exited during launch; see %s' % log.name)
        time.sleep(1)
    proc.terminate()
    raise RuntimeError('sim agent did not come up on port %d' % port)


def _descendant_texts(node):
    out = []
    if node.get('type') == 'lv_label' and node.get('text'):
        out.append(node['text'])
    for child in node.get('children') or []:
        out.extend(_descendant_texts(child))
    return out


def _visible_labels(tree):
    return [n for n in _walk_visible(tree)
            if n.get('type') == 'lv_label' and n.get('text')]


def _walk_visible(node):
    if not (node.get('flags') or {}).get('visible', True):
        return
    yield node
    for child in node.get('children') or []:
        yield from _walk_visible(child)


def _walk_annotated(node, ancestors=(), in_hidden=False):
    flags = node.get('flags') or {}
    hidden = in_hidden or bool(flags.get('hidden'))
    yield node, ancestors, hidden
    for child in node.get('children') or []:
        yield from _walk_annotated(child, ancestors + (node,), hidden)


def _reachable(node, ancestors, vw, vh):
    """Fit each axis through ancestor clips using only available scroll travel."""
    coords = node.get('coords') or {}
    for axis, size, limit, before, after, direction in (
            ('x', 'w', vw, 'left', 'right', 3),
            ('y', 'h', vh, 'top', 'bottom', 12)):
        low = high = coords.get(axis, 0)
        extent = coords.get(size, 0)
        for parent in reversed(ancestors):
            scroll = parent.get('scroll') or {}
            if ((parent.get('flags') or {}).get('scrollable')
                    and scroll.get('dir', 0) & direction):
                low -= max(0, scroll.get(after, 0))
                high += max(0, scroll.get(before, 0))
            box = parent.get('coords') or {}
            low = max(low, box.get(axis, 0) - 2)
            high = min(high, box.get(axis, 0) + box.get(size, limit) - extent + 2)
            if low > high:
                return False
        if high < -2 or low + extent > limit + 2:
            return False
    return True


def _selected_pages(spec, pages):
    requested = set(pages.split(',')) if pages else None
    available = {'%s/%s' % (p['app'], p.get('page', 'root')): p
                 for p in spec['pages']}
    if requested and requested - available.keys():
        raise ValueError('unknown pages: %s' % ', '.join(sorted(requested - available.keys())))
    selected = [p for key, p in available.items() if not requested or key in requested]
    if not any(not s.get('hardware_only') for p in selected for s in p.get('states', [])):
        raise ValueError('selection contains no simulator states')
    return selected


def _wait_idle(runner):
    result = runner.call('sim.wait_idle', {'timeout_ms': 5000})
    if result.get('idle') is not True:
        raise RuntimeError('sim.wait_idle timed out: %r' % result)


def _lint(tree, page, state, vw, vh):
    v = []
    labels = _visible_labels(tree)
    joined = ' | '.join(l.get('text', '') for l in labels)

    for want in state.get('expect_visible', []):
        if want not in joined:
            v.append('missing expected text %r' % want)

    for selector in state.get('single_line', []):
        for lab in labels:
            if selector in lab.get('text', ''):
                lh = (lab.get('styles') or {}).get('line_height')
                h = (lab.get('coords') or {}).get('h', 0)
                if lh and h > lh * 1.5 + 2:
                    v.append('wrapped single-line %r (h=%d line_height=%d): %r'
                             % (selector, h, lh, lab['text']))

    allow_disabled = state.get('allow_disabled', [])
    allow_disabled_types = state.get('allow_disabled_types', [])
    for node, ancestors, in_hidden in _walk_annotated(tree):
        flags = node.get('flags') or {}
        coords = node.get('coords') or {}
        state_flags = node.get('state') or {}
        interactive = (node.get('type') in INTERACTIVE
                       or flags.get('clickable'))
        if not interactive or in_hidden or flags.get('hidden'):
            continue
        if not _reachable(node, ancestors, vw, vh):
            v.append('unreachable control %s at %r' % (node.get('type'), coords))
        if state_flags.get('disabled'):
            texts = _descendant_texts(node)
            if (node.get('type') not in allow_disabled_types and
                    not any(any(a in t for a in allow_disabled)
                            for t in texts)):
                v.append('unexpected disabled control %s %r'
                         % (node.get('type'), texts[:1]))

    for node in _walk_visible(tree):
        if node.get('type') == 'lv_image':
            sid = node.get('image_semantic_id')
            if sid == 'unknown':
                v.append('fallback/unknown image node visible')

    present_images = {n.get('image_semantic_id') for n in _walk_visible(tree)
                      if n.get('type') == 'lv_image'}
    for want in state.get('expect_image', []):
        if want not in present_images:
            v.append('expected image %s not shown' % want)
    return v


def _masked_diff(a, b, masks, tol=24):
    from PIL import Image, ImageChops, ImageDraw
    ia = Image.open(a).convert('RGB')
    ib = Image.open(b).convert('RGB')
    if ia.size != ib.size:
        return 1.0
    diff = ImageChops.difference(ia, ib).convert('L').point(
        lambda p: 255 if p > tol else 0)
    if masks:
        keep = Image.new('L', ia.size, 0)
        d = ImageDraw.Draw(keep)
        for (x, y, w, h) in masks:
            d.rectangle([x, y, x + w - 1, y + h - 1], fill=255)
        diff = Image.composite(Image.new('L', ia.size, 0), diff, keep)
    hist = diff.histogram()
    return sum(hist[1:]) / float(ia.size[0] * ia.size[1])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--spec', default='sim/ci/review/spec.json')
    ap.add_argument('--build', default='build/sim')
    ap.add_argument('--golden', default='sim/ci/golden/ui')
    ap.add_argument('--shots', default='build/sim/review_shots')
    ap.add_argument('--port', type=int, default=5002)
    ap.add_argument('--pages', default=None, help='app/page,app/page filter')
    ap.add_argument('--update', action='store_true',
                    help='write/refresh baselines instead of comparing')
    ap.add_argument('--check', action='store_true',
                    help='compare only (default), failing on missing or '
                         'differing baselines')
    ap.add_argument('--no-launch', action='store_true')
    ap.add_argument('--keep-sim', action='store_true',
                    help='leave the sim running after the review for interactive '
                         'probing instead of sending sim.exit')
    ap.add_argument('--report', default=None)
    ap.add_argument('--max-diff', type=float, default=0.005)
    args = ap.parse_args()
    if args.check and args.update:
        ap.error('--check and --update are mutually exclusive')

    spec = json.load(open(args.spec))
    try:
        spec['pages'] = _selected_pages(spec, args.pages)
    except ValueError as error:
        ap.error(str(error))

    proc = None
    if not args.no_launch and not _port_open('127.0.0.1', args.port):
        proc = _launch_sim(args.build, args.port,
                           os.path.join(args.build, 'review_nvs'), args.shots,
                           '/tmp/mrsd')
    runner = Runner('127.0.0.1', args.port, args.shots, None, args.update)
    try:
        _run_spec(runner, spec, args, proc)
    finally:
        if proc is not None and not args.keep_sim:
            try:
                runner.call('sim.exit')
            except Exception:
                pass
        runner.close()
        if proc is not None and not args.keep_sim:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except Exception:
                proc.kill()


def _run_spec(runner, spec, args, proc):
    vw = (spec.get('viewport') or {}).get('w', 368)
    vh = (spec.get('viewport') or {}).get('h', 448)
    runner.connect()
    os.makedirs(args.golden, exist_ok=True)

    failures = []
    checked = 0
    visual_checked = 0
    geometry_only = []
    hardware_only = []
    for page in spec['pages']:
        key = '%s/%s' % (page['app'], page.get('page', 'root'))
        for state in page.get('states', []):
            if state.get('hardware_only'):
                hardware_only.append(key + '/' + state['name'])
                continue
            checked += 1
            name = '%s__%s__%s' % (page['app'], page.get('page', 'root'),
                                   state['name'])
            try:
                want = state.get('expect_visible', [])
                page_id = page.get('page', 'root')
                tree = None
                for _ in range(3):
                    # Intra-app page navigation can be dropped after a long
                    # session; hop through the app root first, then the page.
                    runner.call('sim.navigate',
                                {'app': page['app'], 'page': 'root'})
                    runner.call('sim.step', {'ms': 33 * 15})
                    if page_id != 'root':
                        runner.call('sim.navigate',
                                    {'app': page['app'], 'page': page_id})
                    for seed in state.get('seed', []):
                        runner._step(seed)
                    runner.call('sim.step', {'ms': 33 * 30})
                    _wait_idle(runner)
                    tree = runner.call('sim.tree')['tree']
                    if not want:
                        break
                    have = ' | '.join(l.get('text', '')
                                      for l in _visible_labels(tree))
                    if all(w in have for w in want):
                        break
                shot = runner.call('sim.screenshot', {'name': name + '.png'})
                path = shot['path']
                for viol in _lint(tree, page, state, vw, vh):
                    failures.append('%s: %s' % (name, viol))
                if state.get('no_golden'):
                    geometry_only.append(name)
                    continue
                golden = os.path.join(args.golden, name + '.png')
                if args.update:
                    shutil.copyfile(path, golden)
                elif os.path.exists(golden):
                    visual_checked += 1
                    frac = _masked_diff(path, golden, state.get('masks', []))
                    if frac > args.max_diff:
                        failures.append('%s: baseline diff %.3f%% > %.3f%%'
                                        % (name, frac * 100,
                                           args.max_diff * 100))
                else:
                    failures.append('%s: no baseline (run --update)' % name)
            except Exception as exc:  # noqa: BLE001
                failures.append('%s: driver error: %s' % (name, exc))

    print('ui-review: %d states checked, %d failures'
          % (checked, len(failures)))
    print('  visual=%d geometry-only=%d hardware-only=%d'
          % (visual_checked, len(geometry_only), len(hardware_only)))
    for f in failures:
        print('   -', f)
    if args.report:
        with open(args.report, 'w') as fh:
            json.dump({'checked': checked, 'visual_checked': visual_checked,
                       'geometry_only': geometry_only, 'hardware_only': hardware_only,
                       'failures': failures}, fh, indent=1)
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
