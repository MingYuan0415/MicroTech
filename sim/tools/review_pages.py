#!/usr/bin/env python3
"""Adversarial UI review harness for the MicroTech LVGL simulator.

Drives the sim agent through a per-page state matrix, dumps the widget tree,
runs geometry/state lint, and captures full-page PNG snapshots for the current
GUI review. It catches wrapped text, off-screen/unreachable controls, washed-out
disabled states, and fallback icons that structure-only assertions and single
happy-path screenshots miss. Snapshots are review artifacts, not versioned
baselines.

Capture is page-wide, not viewport-wide: a page taller than the viewport is
stitched from scroll stripes, so nothing below the fold is exempt. The lint
likewise walks every on-screen node, not only the folded-in ones: `flags.visible`
is LVGL's on-clip test and reads false for content inside a scroll container.

Spec schema (JSON):
  {
    "pages": [
      {"app": "settings", "page": "wifi",
       "states": [
         {"name": "idle_no_profile",
          "seed": [ {"cmd": "set_power", ...}, ... ],   # run_scenarios cmds
          "expect_visible": ["Wi-Fi 开关", "忘记网络"],
          "expect_absent": ["已删除"],  # forbidden stale/false-success copy
          "single_line":    ["IP 地址", "状态"],   # must render as one line
          "balanced_prose": ["预计未来6小时内"],   # multi-line: no orphan tail
          "allow_truncation": ["SSID: "],          # deliberate DOTS prefixes
          "allow_disabled": ["忘记网络"],
          "allow_disabled_types": ["lv_switch"],   # unlabeled intentional
          "expect_image":   [],
          "hardware_only": false}
       ]}
    ]
  }

Usage:
  python3 sim/tools/review_pages.py --spec sim/ci/review/spec.json
      [--pages settings/wifi,...] [--shots DIR]
      [--build build/sim] [--port 0|N] [--no-launch] [--keep-sim]
      [--report out.json]

Exit non-zero on any lint or driver failure. Snapshots are always written to
the selected shots directory and are never compared with a stored baseline.
"""
import argparse
import fcntl
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
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


_launched = []


def _stop_sim(proc, runner):
    """Retire one headless session so the next page starts from boot."""
    if runner is not None:
        try:
            runner.call('sim.exit')
        except Exception:
            pass
        runner.close()
    if proc is not None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()


_PORT_ALLOCATOR_LOCK = None
_PORT_RESERVATION_LOCK = None


def _allocate_port():
    """Return an unused loopback port reserved through simulator startup."""
    global _PORT_ALLOCATOR_LOCK, _PORT_RESERVATION_LOCK
    lock_path = os.path.join(os.environ.get('TMPDIR') or '/tmp',
                             'microtech-sim-port-allocator.lock')
    _PORT_ALLOCATOR_LOCK = open(lock_path, 'w')
    fcntl.flock(_PORT_ALLOCATOR_LOCK.fileno(), fcntl.LOCK_EX)
    while True:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.bind(('127.0.0.1', 0))
            port = sock.getsockname()[1]
        reservation_path = os.path.join(
            os.environ.get('TMPDIR') or '/tmp',
            'microtech-sim-port-%d.lock' % port)
        reservation = open(reservation_path, 'w')
        try:
            fcntl.flock(reservation.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            reservation.close()
            continue
        _PORT_RESERVATION_LOCK = reservation
        return port


def _release_port_allocator():
    global _PORT_ALLOCATOR_LOCK, _PORT_RESERVATION_LOCK
    if _PORT_RESERVATION_LOCK is not None:
        _PORT_RESERVATION_LOCK.close()
        _PORT_RESERVATION_LOCK = None
    if _PORT_ALLOCATOR_LOCK is not None:
        fcntl.flock(_PORT_ALLOCATOR_LOCK.fileno(), fcntl.LOCK_UN)
        _PORT_ALLOCATOR_LOCK.close()
        _PORT_ALLOCATOR_LOCK = None



def _launch_sim(build, port, nvs, shots, sd):
    shutil.rmtree(nvs, ignore_errors=True)
    shutil.rmtree(sd, ignore_errors=True)
    os.makedirs(shots, exist_ok=True)
    log = open(os.path.join(shots, 'review_sim.log'), 'wb')
    # 模拟存储根同样按会话隔离：残留的天气快照会让本次会话一启动就带着
    # 上一次的实况，空态断言随之失真。
    data = os.path.join(shots, 'review_data')
    shutil.rmtree(data, ignore_errors=True)
    environment = dict(os.environ)
    environment['SIM_DATA_DIR'] = data
    proc = subprocess.Popen(
        [os.path.join(build, 'microtech_sim'), '--headless', '--ci',
         '--res-dir', os.path.join(build, 'sim_res_fs'),
         '--nvs-dir', nvs, '--sd-dir', sd, '--out-dir', shots,
         '--agent-port', str(port)], stdout=log, stderr=subprocess.STDOUT,
        env=environment)
    for _ in range(60):
        if _port_open('127.0.0.1', port):
            _release_port_allocator()
            _launched.append(proc)
            return proc
        if proc.poll() is not None:
            _release_port_allocator()
            raise RuntimeError('sim exited during launch; see %s' % log.name)
        time.sleep(1)
    _release_port_allocator()
    proc.terminate()
    raise RuntimeError('sim agent did not come up on port %d' % port)


def _descendant_texts(node):
    out = []
    if node.get('type') == 'lv_label' and node.get('text'):
        out.append(node['text'])
    for child in node.get('children') or []:
        out.extend(_descendant_texts(child))
    return out


def _descendant_semantics(node):
    out = []
    semantic_id = node.get('image_semantic_id')
    if semantic_id and semantic_id != 'dynamic':
        out.append(semantic_id)
    for child in node.get('children') or []:
        out.extend(_descendant_semantics(child))
    return out


# Kinsoku sets mirrored from app_ui_wrap_prose(): a line must not start with
# closing punctuation and must not end with an opener.
FORBIDDEN_START = '，。、；：！？）】」』…'
FORBIDDEN_END = '（【「『《'

LONG_MODE_WRAP = 0


def _lines(lab):
    return [ln['text'] for ln in (lab.get('lines') or []) if ln.get('text')]


def _line_count(lab):
    lines = _lines(lab)
    if lines:
        return len(lines)
    lh = (lab.get('styles') or {}).get('line_height')
    h = (lab.get('coords') or {}).get('h', 0)
    return 2 if (lh and h > lh * 1.5 + 2) else 1


def _clusters(text):
    return sum(1 for b in text.encode('utf-8') if (b & 0xC0) != 0x80)


def _visible_labels(tree):
    return [n for n in _walk_on_screen(tree)
            if n.get('type') == 'lv_label' and n.get('text')]


def _walk_on_screen(node):
    """Yield every node outside a hidden subtree, folded-in or not.

    `flags.visible` is LVGL's on-clip test (`lv_obj_area_is_visible`): a node
    inside a scroll container reports `false` once it sits past the fold, so a
    visible-only walk silently drops exactly the content this review exists to
    check. Only `flags.hidden` marks a genuinely hidden subtree.
    """
    for node, _ancestors, in_hidden in _walk_annotated(node):
        if not in_hidden:
            yield node


MIN_OWNER_SIDE = 64
MIN_OWNER_SPAN = 128
# LVGL treats a drag shorter than its scroll limit as a tap, so a tail at or
# below that limit cannot be brought on screen by a stripe drag.
# Keep in step with LV_INDEV_DEF_SCROLL_LIMIT (sim/lv_conf.h).
MIN_REACHABLE_TRAVEL = 10


def _scroll_owner(tree, vw, vh):
    """Pick the page scroller whose content is clipped by the viewport bottom.

    Prefers a node that starts at the fold (a page content area) over one
    nested inside it, so the outermost page scroller wins; ties break on the
    longest travel. Only a node whose own box reaches the viewport bottom
    qualifies: that is the case where content is cut off by the screen, which a
    scroll stripe can recover. A list whose box sits inside the viewport clips
    its rows by itself, and appending them under the page would misrepresent it.

    Returns `(coords, above, below)`: the current offset and how far the content
    extends above and below the viewport. Below is needed even when it is 0: a
    page seeded at the very bottom reports `below == 0` while still being taller
    than the viewport.
    """
    candidates = []
    for node, _ancestors, in_hidden in _walk_annotated(tree):
        if in_hidden:
            continue
        flags = node.get('flags') or {}
        scroll = node.get('scroll') or {}
        coords = node.get('coords') or {}
        if not flags.get('scrollable'):
            continue
        if not scroll.get('dir', 0) & 12:  # LV_DIR_VER
            continue
        wide, tall = coords.get('w', 0), coords.get('h', 0)
        if min(wide, tall) < MIN_OWNER_SIDE:
            continue
        if max(wide, tall) < MIN_OWNER_SPAN:
            continue
        if (coords.get('y', 0) + tall) < vh:
            # Only a box that fully reaches the viewport bottom leaves no canvas
            # row unpainted.
            continue
        below = max(0, scroll.get('bottom', 0))
        above = max(0, scroll.get('top', 0))
        if (below + above) <= 0:
            continue
        anchor = 0 if coords.get('y', 0) >= vh * 0.05 else 1
        candidates.append((anchor, -(below + above), coords, above, below))
    if not candidates:
        return None
    best = min(candidates, key=lambda c: c[:2])
    return best[2], best[3], best[4]


def _capture_full_page(runner, name, vw, vh, wait_idle=True):
    """Screenshot the whole scrollable page, stitching past-the-fold rows.

    Each stripe is pasted at `offset + box.y`, so the content band always lands
    on the same canvas rows and later stripes fill only rows no earlier stripe
    reached. Returns (path, note); `note` is None when the capture was a plain
    screenshot.
    """
    from PIL import Image

    def _shot(shot_name):
        return runner.call('sim.screenshot',
                           {'name': shot_name,
                            'wait_idle': wait_idle})['path']

    tree = runner.call('sim.tree')['tree']
    for node in _walk_on_screen(tree):
        flags = node.get('flags') or {}
        box = node.get('coords') or {}
        if (flags.get('floating') and flags.get('clickable') and
                box.get('w', 0) >= vw and box.get('h', 0) >= vh):
            return _shot(name + '.png'), 'modal surface: single frame'
    owner = _scroll_owner(tree, vw, vh)
    if owner is None:
        return _shot(name + '.png'), None
    box, _offset, below = owner

    def _plain():
        return _shot(name + '.png'), None

    if below <= MIN_REACHABLE_TRAVEL and _offset <= 0:
        # Content fits, or its whole tail is within LVGL's scroll limit and so
        # is unreachable by construction: keep the faithful single frame.
        return _plain()
    remaining = below
    if _offset > 0:
        # Anchor at the top so the canvas has no unpainted rows. Skipped when
        # already at the top: a pointless downward drag just bounces off the
        # elastic edge, and reading the tree mid-settle reports `below == 0`.
        anchored = _scroll_to_top(runner, box, vw, vh)
        if anchored is None:
            # Without a verified top anchor a composite would be misaligned;
            # the single frame is the honest artifact instead.
            frame, _ = _plain()
            return frame, 'full-page skipped: page top not reachable'
        remaining = anchored[2]

    stripes = []
    y_off = 0
    for index in range(40):
        stripes.append((y_off, _shot('%s__%02d.png' % (name, index))))
        if remaining <= 0:
            break
        # Over-drag on purpose: lv_obj_scroll_by_bounded clamps to the real
        # bottom, while a hop under LVGL's 10 px scroll limit would be swallowed
        # as a tap and leave the last few pixels uncaptured.
        _scroll_owner_drag(runner, box, max(40, box['h'] - 24))
        runner.call('sim.wait_idle', {'timeout_ms': 4000})
        fresh = _scroll_owner(runner.call('sim.tree')['tree'], vw, vh)
        if fresh is None or fresh[1] <= y_off:
            break
        remaining, y_off = fresh[2], fresh[1]
    else:
        raise RuntimeError('full-page capture did not settle after 40 stripes')
    if remaining > MIN_REACHABLE_TRAVEL:
        # The stripe loop stopped with content still reachable; a shorter canvas
        # would silently drop it.
        raise RuntimeError('full-page capture stopped %d px short of the bottom'
                           % remaining)

    canvas = Image.new('RGB', (vw, vh + y_off), (0, 0, 0))
    canvas.paste(Image.open(stripes[0][1]).convert('RGB'), (0, 0))
    reached = vh
    for offset, path in stripes[1:]:
        # Frame row (top - offset) shows the content row that belongs on canvas
        # row `top`; rows the first stripe already covered are left untouched.
        first = offset + box['y']
        if first > reached:
            # A stripe that advanced more than the box height would leave the
            # rows between them unpainted, and that hole would be baked into the
            # snapshot as black.
            raise RuntimeError('full-page capture left %d canvas rows unpainted'
                               % (first - reached))
        top = max(first, reached)
        bottom = offset + box['y'] + box['h']
        if top < bottom:
            canvas.paste(Image.open(path).convert('RGB').crop(
                (0, top - offset, vw, bottom - offset)), (0, top))
            reached = bottom
    if reached != vh + y_off:
        raise RuntimeError('full-page capture painted %d of %d canvas rows'
                           % (reached, vh + y_off))
    out = os.path.join(os.path.dirname(stripes[0][1]), name + '_full.png')
    canvas.save(out)
    for _offset, path in stripes:
        try:
            os.unlink(path)
        except OSError:
            pass
    return out, ('page content %d px in a %d px viewport (+%d px folded)'
                 % (box['h'] + y_off, box['h'], y_off))


def _scroll_owner_drag(runner, box, distance):
    """Drag the owner's content by `distance` panel pixels (positive scrolls up).

    The finger travels the long way inside the panel for the given direction:
    scrolling the content up starts near the panel bottom, scrolling it back to
    the top starts near the panel top. Starting at the far end for a negative
    distance would leave `start == end`, which LVGL reads as a tap and never
    scrolls. The press x sits inside the content rows: a press in the page's
    side padding does not start a scroll at all (measured: x >= 340 on a
    368-wide page leaves `scroll.y` unchanged), and the left edge strip belongs
    to the edge-back gesture. Rows are inset from the box, so their horizontal
    middle is off any trailing switch or chevron.
    """
    x = box['x'] + box['w'] // 2
    if distance >= 0:
        start = min(box['y'] + box['h'] - 12, 440)
        end = start - distance
    else:
        start = max(box['y'] + 12, 8)
        end = start - distance
    start = max(8, min(440, start))
    end = max(8, min(440, end))
    moves = max(4, min(12, abs(distance) // 24))
    runner.call('sim.touch', {'action': 'down', 'x': x, 'y': start})
    for i in range(1, moves + 1):
        runner.call('sim.touch', {'action': 'move', 'x': x,
                                  'y': start + (end - start) * i // moves})
        runner.call('sim.step', {'ms': 33})
    runner.call('sim.touch', {'action': 'up', 'x': x, 'y': end})
    runner.call('sim.step', {'ms': 33 * 4})


def _scroll_to_top(runner, box, vw, vh):
    """Anchor the page scroller at its top, undoing any mid-scroll seed.

    The finger can only travel one panel height per drag, so a deep seed needs
    several passes; over-dragging is safe because the scroll clamps at the top.
    Returns the owner reading once offset 0 is reached, else None: a caller that
    assumed 0 anyway would misalign every stripe.
    """
    for _ in range(6):
        fresh = _scroll_owner(runner.call('sim.tree')['tree'], vw, vh)
        if fresh is None:
            return None
        if fresh[1] <= 0:
            return fresh
        _scroll_owner_drag(runner, box, -box['h'])
        runner.call('sim.wait_idle', {'timeout_ms': 4000})
    return None


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


def _build_product(build):
    """Return the product id written by the simulator build, if available."""
    profile = os.path.join(build, 'mt_product_profile.h')
    try:
        with open(profile, encoding='utf-8', errors='replace') as handle:
            for line in handle:
                prefix = '#define MT_PRODUCT_NAME "'
                if line.startswith(prefix):
                    return line[len(prefix):].split('"', 1)[0]
    except OSError:
        pass
    return ''


def _apply_product_overrides(spec, product):
    """Apply the selected product's review expectations to the shared spec."""
    overrides = (spec.get('product_overrides') or {}).get(product, {})
    if not overrides:
        return spec
    skip_pages = set(overrides.get('skip_pages') or [])
    state_overrides = overrides.get('states') or {}
    pages = []
    for page in spec['pages']:
        page_key = '%s/%s' % (page['app'], page.get('page', 'root'))
        if page_key in skip_pages:
            continue
        for state in page.get('states', []):
            state_key = '%s/%s' % (page_key, state['name'])
            override = state_overrides.get(state_key) or {}
            remove = set(override.get('expect_visible_remove') or [])
            if remove:
                state['expect_visible'] = [
                    item for item in state.get('expect_visible', [])
                    if item not in remove
                ]
            additions = override.get('allow_truncation_add') or []
            if additions:
                state['allow_truncation'] = list(
                    state.get('allow_truncation', [])
                ) + additions
        pages.append(page)
    spec['pages'] = pages
    return spec


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


def _lint_labels(tree, state):
    """Label display rules: one-line identity, clipping, truncation, prose.

    Every rule reads the rendered structure (long mode, line count, per-line
    text) rather than the raw string, because LVGL's wrap never touches
    `text` and DOTS rewrites it in place.
    """
    v = []
    allow_truncation = tuple(state.get('allow_truncation', []))
    balanced = state.get('balanced_prose', [])
    single_line = state.get('single_line', [])

    for node, ancestors, in_hidden in _walk_annotated(tree):
        if in_hidden or (node.get('type') != 'lv_label' or not node.get('text')):
            continue
        text = node['text']
        long_mode = node.get('long_mode', LONG_MODE_WRAP)
        lines = _lines(node)
        count = _line_count(node)

        for selector in single_line:
            if selector in text and count != 1:
                v.append('wrapped single-line %r (%d lines): %r'
                         % (selector, count, text))

        if 'lines_complete' not in node or node['lines_complete'] is not True:
            v.append('incomplete rendered lines %r' % text)

        if long_mode == LONG_MODE_WRAP:
            if node.get('content_h') is None:
                v.append('missing label content_h %r' % text)
            elif lines:
                lh = (node.get('styles') or {}).get('line_height') or 0
                ls = node.get('line_space') or 0
                content_h = node['content_h']
                need = len(lines) * lh + (len(lines) - 1) * ls if lh else 0
                if lh and len(lines) > 1 and need > content_h + 2:
                    v.append('clipped text %r: %d lines need %d px, content is %d px'
                             % (text, len(lines), need, content_h))
            coords = node.get('coords') or {}
            for parent in reversed(ancestors):
                flags = parent.get('flags') or {}
                scroll = parent.get('scroll') or {}
                if flags.get('scrollable') and scroll.get('dir', 0) & 12:
                    break
                box = parent.get('coords') or {}
                if (box.get('y', 0) + box.get('h', 0)
                        < coords.get('y', 0) + coords.get('h', 0) - 2):
                    v.append('clipped by parent %r: label ends at %d, parent '
                             'ends at %d'
                             % (text,
                                coords.get('y', 0) + coords.get('h', 0),
                                box.get('y', 0) + box.get('h', 0)))
                    break
        if text.endswith('...') and not text.startswith(allow_truncation):
            v.append('truncated text %r' % text)

        for selector in balanced:
            if selector not in text or count < 2 or not lines:
                continue
            last = lines[-1]
            if _clusters(last) < 3:
                v.append('orphan tail %r on %r' % (last, selector))
            for i, line in enumerate(lines):
                if i > 0 and line[0] in FORBIDDEN_START:
                    v.append('line opens with %r: %r' % (line[0], line))
                if i < len(lines) - 1 and line[-1] in FORBIDDEN_END:
                    v.append('line ends with %r: %r' % (line[-1], line))
    return v


def _lint(tree, page, state, vw, vh):
    v = []
    labels = _visible_labels(tree)
    joined = ' | '.join(l.get('text', '') for l in labels)

    for want in state.get('expect_visible', []):
        if want not in joined:
            v.append('missing expected text %r' % want)
    for forbidden in state.get('expect_absent', []):
        if forbidden in joined:
            v.append('unexpected text %r' % forbidden)

    v.extend(_lint_labels(tree, state))

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
            semantics = _descendant_semantics(node)
            allowed = (any(any(a in t for a in allow_disabled)
                           for t in texts) or
                       any(s in allow_disabled for s in semantics))
            if node.get('type') not in allow_disabled_types and not allowed:
                v.append('unexpected disabled control %s %r'
                         % (node.get('type'), texts[:1]))

    for node in _walk_on_screen(tree):
        if node.get('type') == 'lv_image':
            sid = node.get('image_semantic_id')
            if sid == 'unknown':
                v.append('fallback/unknown image node visible')

    present_images = {n.get('image_semantic_id') for n in _walk_on_screen(tree)
                      if n.get('type') == 'lv_image'}
    for want in state.get('expect_image', []):
        if want not in present_images:
            v.append('expected image %s not shown' % want)
    return v


def _session_ownership(no_launch, port_busy, port):
    """Decide how this run gets a sim agent, or fail instead of attaching.

    A busy port means some other session already owns the agent: a leftover dev
    sim or a second review run. Attaching to it silently would drive a tree
    someone else navigates, with per-page isolation off and no fixture seed, so
    the captures stop describing the page under review and the failures move
    between runs. run_ci.sh fails fast for the same reason; an operator who
    really wants the external session asks for it explicitly with --no-launch.

    Returns True when the caller must launch and own a sim process. Raises
    ValueError when the port is occupied and --no-launch was not requested.
    """
    if no_launch:
        return False
    if port_busy:
        raise ValueError(
            'port %d already has a live agent (stop the dev/review session '
            'or pass --no-launch to drive it deliberately)' % port)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--spec', default='sim/ci/review/spec.json')
    ap.add_argument('--build', default='build/sim')
    ap.add_argument('--shots', default=None)
    ap.add_argument('--port', type=int, default=0,
                    help='agent port; 0 allocates an isolated loopback port')
    ap.add_argument('--pages', default=None, help='app/page,app/page filter')
    ap.add_argument('--no-launch', action='store_true')
    ap.add_argument('--keep-sim', action='store_true',
                    help='leave the sim running after the review for interactive '
                         'probing instead of sending sim.exit')
    ap.add_argument('--report', default=None)
    args = ap.parse_args()
    if args.port == 0:
        if args.no_launch:
            ap.error('--no-launch requires an explicit --port')
        args.port = _allocate_port()
    if args.shots is None:
        os.makedirs(args.build, exist_ok=True)
        args.shots = tempfile.mkdtemp(prefix='review-', dir=args.build)

    spec = json.load(open(args.spec))
    spec = _apply_product_overrides(spec, _build_product(args.build))
    try:
        spec['pages'] = _selected_pages(spec, args.pages)
    except ValueError as error:
        ap.error(str(error))

    proc = None
    try:
        owns_sim = _session_ownership(args.no_launch,
                                      _port_open('127.0.0.1', args.port),
                                      args.port)
    except ValueError as error:
        ap.error(str(error))
    sd_dir = None
    if owns_sim:
        # sd_storage_service keeps its mount path in a 64-byte buffer, so the
        # review's long screenshot/NVS tree cannot be used as the card root.
        sd_dir = tempfile.mkdtemp(prefix='mt-review-sd-')
        proc = _launch_sim(args.build, args.port,
                           os.path.join(args.shots, 'review_nvs'), args.shots,
                           sd_dir)
    runner = Runner('127.0.0.1', args.port, args.shots)
    try:
        _run_spec(runner, spec, args, proc, owns_sim, sd_dir)
    finally:
        if not args.keep_sim:
            for live in _launched:
                _stop_sim(live, None)
            _launched[:] = []
            if sd_dir:
                shutil.rmtree(sd_dir, ignore_errors=True)
        runner.close()


def _viewport_from_ping(ping):
    """Return the live simulator viewport from a sim.ping response."""
    try:
        vw = int(ping['width'])
        vh = int(ping['height'])
    except (KeyError, TypeError, ValueError) as error:
        raise RuntimeError('sim.ping did not report a valid viewport') from error
    if vw <= 0 or vh <= 0:
        raise RuntimeError('invalid simulator viewport %dx%d' % (vw, vh))
    return vw, vh


def _run_spec(runner, spec, args, proc, owns_sim=False, sd_dir=None):
    runner.connect()
    vw, vh = _viewport_from_ping(runner.call('sim.ping'))
    os.makedirs(args.shots, exist_ok=True)

    failures = []
    checked = 0
    snapshots = 0
    hardware_only = []
    page_index = 0
    for page in spec['pages']:
        key = '%s/%s' % (page['app'], page.get('page', 'root'))
        # One sim process per page. States seed the medium, run recordings and
        # leave services in whatever mode they were testing; a state that ends
        # faulted or unmounted would otherwise be inherited by the next page,
        # whose capture then carries the previous page's state and no longer
        # describes its own page. This mirrors the per-scenario process model
        # in sim/ci/run_ci.sh.
        #
        # The external --no-launch path keeps a single session: that mode
        # exists to drive a sim the operator already controls.
        if owns_sim and page_index > 0 and proc is not None:
            _stop_sim(proc, runner)
            proc = _launch_sim(args.build, args.port,
                               os.path.join(args.build, 'review_nvs'),
                               args.shots, sd_dir or '/tmp/mrsd')
            runner = Runner('127.0.0.1', args.port, args.shots)
            runner.connect()
        if owns_sim:
            # A fresh process starts with an empty card. States whose seeds
            # only navigate (files/detail, the settings pages) relied on a
            # sibling page creating this tree first; recreate it here so a
            # page never depends on another page having run before it. The
            # first session needs the same seed as a relaunched one: a subset
            # run (`--pages`) starts at page_index 0, and its files states
            # reach for this tree without any sibling page having created it.
            runner.call('sim.sd', {'action': 'fixture'})
        page_index += 1
        for state in page.get('states', []):
            if state.get('hardware_only'):
                hardware_only.append(key + '/' + state['name'])
                continue
            if state.get('fresh') and owns_sim and proc is not None:
                _stop_sim(proc, runner)
                proc = _launch_sim(args.build, args.port,
                                   os.path.join(args.build, 'review_nvs'),
                                   args.shots, sd_dir or '/tmp/mrsd')
                runner = Runner('127.0.0.1', args.port, args.shots)
                runner.connect()
                runner.call('sim.sd', {'action': 'fixture'})
            checked += 1
            name = '%s__%s__%s' % (page['app'], page.get('page', 'root'),
                                   state['name'])
            try:
                want = state.get('expect_visible', [])
                page_id = page.get('page', 'root')
                tree = None
                for seed in state.get('pre_seed', []):
                    runner._step(seed)
                for _ in range(3):
                    # Intra-app page navigation can be dropped after a long
                    # session; hop through the app root first, then the page.
                    runner.call('sim.navigate',
                                {'app': page['app'], 'page': 'root'})
                    if page_id != 'root':
                        navigation = {'app': page['app'], 'page': page_id}
                        navigation.update(state.get('navigate_args') or {})
                        runner.call('sim.navigate', navigation)
                    runner.call('sim.step', {'ms': 33 * 10})
                    # Seed only after the target page is mounted. A preceding
                    # mount-time service refresh can otherwise replace a newly
                    # installed fixture before the capture observes it.
                    for seed in state.get('seed', []):
                        runner._step(seed)
                    runner.call('sim.step', {'ms': 33 * 30})
                    if state.get('animated'):
                        # Infinite widget animation never reports idle; sample a
                        # stable pair of frames instead of waiting for it.
                        runner.call('sim.step', {'ms': 33 * 2})
                    else:
                        _wait_idle(runner)
                    tree = runner.call('sim.tree')['tree']
                    if not want:
                        break
                    have = ' | '.join(l.get('text', '')
                                      for l in _visible_labels(tree))
                    if all(w in have for w in want):
                        break
                path, folded = _capture_full_page(runner, name, vw, vh,
                                                  not state.get('animated'))
                snapshot = os.path.join(args.shots, os.path.basename(path))
                if os.path.abspath(path) != os.path.abspath(snapshot):
                    shutil.copyfile(path, snapshot)
                snapshots += 1
                for viol in _lint(tree, page, state, vw, vh):
                    failures.append('%s: %s' % (name, viol))
                if folded is not None:
                    print('  %s: %s' % (name, folded))
            except Exception as exc:  # noqa: BLE001
                failures.append('%s: driver error: %s' % (name, exc))

    print('ui-review: %d states checked, %d snapshots, %d failures'
          % (checked, snapshots, len(failures)))
    print('  hardware-only=%d' % len(hardware_only))
    for f in failures:
        print('   -', f)
    if args.report:
        with open(args.report, 'w') as fh:
            json.dump({
                'checked': checked,
                'snapshots': snapshots,
                'hardware_only': hardware_only,
                'failures': failures,
                'shots': os.path.abspath(args.shots),
                'viewport': {'w': vw, 'h': vh},
            }, fh, indent=1)
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
