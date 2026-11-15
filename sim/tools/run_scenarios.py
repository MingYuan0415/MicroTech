#!/usr/bin/env python3
"""Scenario runner for the MicroTech LVGL simulator.

Scenario JSON:
  {
    "name": "...",
    "timeout_seconds": 60,
    "checks": {"clock_visible": {"description": "..."}},
    "check_coverage": ["clock_visible"],
    "failure_evidence": {"screenshot": true, "tree": true},
    "steps": [
      {"cmd": "navigate", "app": "clock"},
      {"cmd": "wait_text", "text": "时钟", "timeout_ms": 3000,
       "check": "clock_visible"},
      {"cmd": "step", "ms": 33, "times": 20},
      {"cmd": "wait_idle", "timeout_ms": 4000},
      {"cmd": "set_power", "voltage": 3900, "pct": 66, "charging": false},
      {"cmd": "screenshot", "name": "clock.png"},
      {"cmd": "tree_assert", "contains": {"type": "lv_label", "text": "时钟"}},
      {"cmd": "touch", "action": "down", "x": 20, "y": 30},
      {"cmd": "drag", "x0": 184, "y0": 300, "x1": 184, "y1": 160,
       "hold_steps": 1}   # hold the press one frame so it lands at (x0, y0)
    ]
  }

`wait_text` polls visible text by default; `wait_tree` polls a `contains` /
`absent` map. Root `checks` / `check_coverage` require each declared check to be
claimed by a passing step; missing, failed, or unplanned checks fail the
scenario. On failure the runner writes `failure_<scenario>_<step>.png` and
`.tree.json` under `--out-dir`; evidence capture is a separate best-effort
15-second budget per artifact and can outlive the scenario budget.

tree_assert: contains asserts that a node matching ALL given key/values exists;
absent asserts that no node matches ALL given key/values
(coords/flags/state are checked via the flattened dotted key, e.g.
 "coords.x" style nesting is supported by walking dicts; a numeric key may
 match a range with {"min": N, "max": M}).

flybirds_restart_regression: from PLAYING, fly by current bird/pipe geometry
for at most 360 single 33 ms frames to score 1, wait at most 120 frames for
settlement, restart with a short tap, and prove HUD/settlement return to 0
while highest remains 1. Numeric assertions are scoped to visible containers.
Saves flybirds_over.png, flybirds_restart_zero.png, and
flybirds_second_over_zero.png; takes no parameters.

Usage:
  python3 sim/tools/run_scenarios.py sim/ci/scenarios/*.json
      [--report out.json] [--host H] [--port P]
"""
import argparse
import glob
import json
import os
import re
import shutil
import socket
import sys
import time  # noqa: E401

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
MT_ROOT = os.path.dirname(os.path.dirname(TOOLS_DIR))
sys.path.insert(0, TOOLS_DIR)
from simctl import rpc  # noqa: E402


def _flat_matches(node, match):
    for key, want in match.items():
        cur = node
        ok = True
        for part in key.split('.'):
            if not isinstance(cur, dict) or part not in cur:
                ok = False
                break
            cur = cur[part]
        if not ok:
            return False
        if isinstance(want, dict):
            value = cur if isinstance(cur, (int, float)) else None
            if value is None:
                return False
            if 'min' in want and value < want['min']:
                return False
            if 'max' in want and value > want['max']:
                return False
        elif cur != want:
            return False
    return True


def _walk(node):
    yield node
    for child in node.get('children', []):
        yield from _walk(child)
    for child in node.get('overlays', []):
        yield from _walk(child)


def _walk_actionable(node, blocked=False):
    blocked = blocked or node.get('flags', {}).get('hidden', False) or node.get('state', {}).get('disabled', False)
    if not blocked:
        yield node
    for child in node.get('children', []) + node.get('overlays', []):
        yield from _walk_actionable(child, blocked)


def _walk_visible(node, hidden=False):
    hidden = hidden or node.get('flags', {}).get('hidden', False)
    if not hidden and node.get('flags', {}).get('visible', True):
        yield node
    for child in node.get('children', []) + node.get('overlays', []):
        yield from _walk_visible(child, hidden)


def _safe_evidence_name(name):
    return re.sub(r'[^A-Za-z0-9_.-]+', '_', name).strip('._') or 'scenario'


def _text_targets(node, text, ancestors=(), blocked=False):
    """Yield the nearest actionable ancestor (or the text node itself)."""
    blocked = blocked or node.get('flags', {}).get('hidden', False) or node.get('state', {}).get('disabled', False)
    target = node if node.get('flags', {}).get('clickable') else None
    for ancestor in reversed(ancestors):
        if ancestor.get('flags', {}).get('clickable'):
            target = ancestor
            break
    if not blocked and node.get('text') == text and target is not None:
        yield node, target
    for child in node.get('children', []) + node.get('overlays', []):
        yield from _text_targets(child, text, ancestors + (node,), blocked)


class ScenarioTimeout(AssertionError):
    """The scenario exceeded its root ``timeout_seconds`` budget."""


class Runner:
    def __init__(self, host, port, out_dir):
        self.host, self.port = host, port
        self.out_dir = out_dir
        self.sock = None
        self._lost_connection = False
        self.width = 368
        self.height = 448
        self.deadline = None
        self._timeout_seconds = 0.0
        self._scenario_name = 'scenario'
        self._checks = {}
        self._check_states = {}
        self._failure_evidence = {}

    def connect(self):
        self.sock = socket.create_connection((self.host, self.port), timeout=60)
        self._lost_connection = False
        ping = self.call('sim.ping')
        self.width = int(ping.get('width', self.width))
        self.height = int(ping.get('height', self.height))

    def _rpc_reply(self, method, params=None, timeout=None,
                   ignore_deadline=False):
        """Send one RPC while honoring the scenario and socket deadlines."""
        budget = timeout
        if not ignore_deadline and self.deadline is not None:
            remaining = self.deadline - time.monotonic()
            if remaining <= 0:
                raise ScenarioTimeout('scenario timeout after %.1fs' %
                                      self._timeout_seconds)
            budget = remaining if budget is None else min(budget, remaining)
        if budget is None:
            budget = 60.0
        if self.sock is None:
            if self._lost_connection:
                # Reconnecting is part of the same RPC budget. A fixed 15s
                # connect could otherwise outlive a short wait/scenario
                # deadline and turn a precise timeout into a long stall.
                self._reconnect(timeout=budget)
            else:
                self._lost_connection = True
                raise RuntimeError('simulator connection is not established')
        old_timeout = self.sock.gettimeout()
        self.sock.settimeout(max(0.001, budget))
        try:
            return rpc(self.sock, method, params)
        except (OSError, RuntimeError, ValueError):
            # A timed-out RPC may still produce a late response. Reusing this
            # socket would deliver that response to the next request (often
            # failure evidence), so drop it and let the caller reconnect.
            self._lost_connection = True
            self.close()
            raise
        finally:
            if self.sock is not None:
                self.sock.settimeout(old_timeout)

    def call(self, method, params=None, timeout=None, ignore_deadline=False):
        reply = self._rpc_reply(method, params, timeout, ignore_deadline)
        if not reply.get('ok'):
            raise RuntimeError('%s failed: %s' % (method, reply))
        return reply['result']

    def _check_deadline(self):
        if (self.deadline is not None and
                time.monotonic() >= self.deadline):
            raise ScenarioTimeout('scenario timeout after %.1fs' %
                                  self._timeout_seconds)

    def close(self):
        if self.sock:
            self.sock.close()
            self.sock = None

    def _reconnect(self, timeout=None):
        self.close()
        connect_timeout = 15.0 if timeout is None else max(
            0.001, min(15.0, float(timeout)))
        self.sock = socket.create_connection(
            (self.host, self.port), timeout=connect_timeout)
        self._lost_connection = False

    def _tree(self, include_layers=False, timeout=None):
        return self.call('sim.tree',
                         {'include_layers': bool(include_layers)},
                         timeout=timeout)['tree']

    def _find(self, query, include_layers=False, limit=1, visible_only=False,
              timeout=None):
        return self.call('sim.find', {
            'query': query,
            'include_layers': bool(include_layers),
            'limit': int(limit),
            'visible_only': bool(visible_only),
        }, timeout=timeout)

    def _find_or_none(self, query, include_layers=False, limit=1,
                      visible_only=False, timeout=None):
        try:
            result = self._find(query, include_layers, limit, visible_only,
                                timeout=timeout)
        except RuntimeError as exc:
            message = str(exc)
            if 'unsupported query' in message or 'unknown method' in message:
                return None
            raise
        return result if 'total' in result else None

    @staticmethod
    def _wait_budget(deadline):
        """Return the remaining local wait budget for one RPC."""
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return 0.0
        return max(0.001, remaining)

    def _wait_tree(self, step):
        timeout_ms = int(step.get('timeout_ms', 5000))
        interval_ms = max(1, int(step.get('interval_ms', 33)))
        include_layers = bool(step.get('include_layers', False))
        assert timeout_ms > 0, 'wait_tree timeout_ms must be greater than zero'
        deadline = time.monotonic() + timeout_ms / 1000.0
        present = step.get('contains')
        absent = step.get('absent')
        assert isinstance(present, dict) or isinstance(absent, dict), \
            'wait_tree needs a contains or absent map'
        while True:
            budget = self._wait_budget(deadline)
            if budget <= 0.0:
                self._raise_wait_tree_timeout(present, absent, present_ok=True,
                                              absent_ok=True)
            present_ok = True
            absent_ok = True
            result = None
            if present is not None:
                result = self._find_or_none(present,
                                            include_layers=include_layers,
                                            limit=1, timeout=budget)
            if result is None and present is not None:
                budget = self._wait_budget(deadline)
                if budget <= 0.0:
                    self._raise_wait_tree_timeout(
                        present, absent, present_ok=True, absent_ok=True)
                tree = self._tree(include_layers, timeout=budget)
                present_ok = any(_flat_matches(node, present)
                                 for node in _walk(tree))
            elif result is not None and present is not None:
                present_ok = int(result.get('total', 0)) > 0
            if absent is not None:
                budget = self._wait_budget(deadline)
                if budget <= 0.0:
                    self._raise_wait_tree_timeout(
                        present, absent, present_ok, absent_ok=True)
                absent_result = self._find_or_none(
                    absent, include_layers=include_layers, limit=1,
                    timeout=budget)
                if absent_result is None:
                    budget = self._wait_budget(deadline)
                    if budget <= 0.0:
                        self._raise_wait_tree_timeout(
                            present, absent, present_ok, absent_ok=True)
                    tree = self._tree(include_layers, timeout=budget)
                    absent_ok = not any(_flat_matches(node, absent)
                                        for node in _walk(tree))
                else:
                    absent_ok = int(absent_result.get('total', 0)) == 0
            if present_ok and absent_ok:
                return
            budget = self._wait_budget(deadline)
            if budget <= 0.0:
                self._raise_wait_tree_timeout(present, absent, present_ok,
                                              absent_ok)
            self.call('sim.step', {'ms': interval_ms}, timeout=budget)

    @staticmethod
    def _raise_wait_tree_timeout(present, absent, present_ok, absent_ok):
        if present is not None and not present_ok:
            raise AssertionError(
                'wait_tree timeout: no node matches %s' %
                json.dumps(present, ensure_ascii=False))
        if absent is not None and not absent_ok:
            raise AssertionError(
                'wait_tree timeout: unexpected node matches %s' %
                json.dumps(absent, ensure_ascii=False))
        if present is not None:
            raise AssertionError(
                'wait_tree timeout: no node matches %s' %
                json.dumps(present, ensure_ascii=False))
        raise AssertionError(
            'wait_tree timeout: unexpected node matches %s' %
            json.dumps(absent, ensure_ascii=False))

    def _wait_text(self, step):
        text = step['text']
        timeout_ms = int(step.get('timeout_ms', 5000))
        interval_ms = max(1, int(step.get('interval_ms', 33)))
        include_layers = bool(step.get('include_layers', False))
        visible_only = bool(step.get('visible_only', True))
        assert timeout_ms > 0, 'wait_text timeout_ms must be greater than zero'
        deadline = time.monotonic() + timeout_ms / 1000.0
        while True:
            budget = self._wait_budget(deadline)
            if budget <= 0.0:
                raise AssertionError('wait_text timeout: %r not found' % text)
            result = self._find_or_none({'text': text},
                                        include_layers=include_layers,
                                        limit=1,
                                        visible_only=visible_only,
                                        timeout=budget)
            if result is not None:
                found = int(result.get('total', 0)) > 0
            else:
                budget = self._wait_budget(deadline)
                if budget <= 0.0:
                    raise AssertionError(
                        'wait_text timeout: %r not found' % text)
                tree = self._tree(include_layers, timeout=budget)
                nodes = (_walk_visible(tree) if visible_only
                         else _walk(tree))
                found = any(node.get('text') == text for node in nodes)
            if found:
                return
            budget = self._wait_budget(deadline)
            if budget <= 0.0:
                raise AssertionError('wait_text timeout: %r not found' % text)
            self.call('sim.step', {'ms': interval_ms}, timeout=budget)

    def _mark_check(self, check, status, step_index):
        if not check:
            return
        if check not in self._checks:
            self._check_states.setdefault(
                check, {'status': 'unplanned', 'step': step_index})
            return
        state = self._check_states[check]
        if state['status'] == 'fail':
            return
        state['status'] = status
        state['step'] = step_index

    def _coverage(self):
        if not self._checks and not self._check_states:
            return None
        for check in self._checks:
            self._check_states.setdefault(
                check, {'status': 'not_run', 'step': None})
        counts = {'pass': 0, 'fail': 0, 'not_run': 0, 'unplanned': 0}
        for state in self._check_states.values():
            counts[state['status']] = counts.get(state['status'], 0) + 1
        return {
            'planned': len(self._checks),
            'passed': counts['pass'],
            'failed': counts['fail'],
            'not_run': counts['not_run'],
            'unplanned': counts['unplanned'],
            'ok': (counts['pass'] == len(self._checks) and
                   counts['fail'] == 0 and counts['not_run'] == 0 and
                   counts['unplanned'] == 0),
        }

    def _capture_failure_evidence(self, step_index):
        options = self._failure_evidence
        if not options:
            return None
        stem = '%s_step_%d' % (_safe_evidence_name(self._scenario_name),
                               step_index)
        evidence = {'screenshot': None, 'tree': None, 'errors': []}
        try:
            os.makedirs(self.out_dir, exist_ok=True)
        except OSError as exc:
            evidence['errors'].append('output directory: %s' % exc)
            return evidence
        if self.sock is None and self._lost_connection:
            try:
                self._reconnect()
            except OSError as exc:
                evidence['errors'].append('reconnect: %s' % exc)
                return evidence
        if options.get('screenshot', True):
            try:
                result = self.call('sim.screenshot',
                                   {'name': 'failure_%s.png' % stem,
                                    'wait_idle': False},
                                   timeout=15, ignore_deadline=True)
                source = result.get('path')
                if source and os.path.isfile(source):
                    target = os.path.join(self.out_dir,
                                          os.path.basename(source))
                    if os.path.abspath(source) != os.path.abspath(target):
                        shutil.copy2(source, target)
                    evidence['screenshot'] = target
                else:
                    evidence['screenshot'] = source
            except Exception as exc:  # evidence must not mask the failure
                evidence['errors'].append('screenshot: %s' % exc)
        if options.get('tree', True):
            try:
                tree = self.call(
                    'sim.tree',
                    {'include_layers': options.get('include_layers', True)},
                    timeout=15, ignore_deadline=True)['tree']
                path = os.path.join(self.out_dir, 'failure_%s.tree.json' % stem)
                with open(path, 'w', encoding='utf-8') as handle:
                    json.dump(tree, handle, ensure_ascii=False)
                evidence['tree'] = path
            except Exception as exc:  # evidence must not mask the failure
                evidence['errors'].append('tree: %s' % exc)
        return evidence

    def run(self, scenario):
        failures = []
        name = scenario.get('name', 'scenario')
        self.deadline = None
        self._scenario_name = name
        self._checks = {}
        raw_checks = scenario.get('checks') or {}
        if isinstance(raw_checks, list):
            raw_checks = {item: {'description': item} for item in raw_checks}
        if isinstance(raw_checks, dict):
            for check_id, config in raw_checks.items():
                if isinstance(config, str):
                    config = {'description': config}
                if not isinstance(config, dict):
                    config = {}
                self._checks[str(check_id)] = config
        self._check_states = {
            check: {'status': 'not_run', 'step': None}
            for check in self._checks
        }
        declared_checks = dict(self._checks)
        planned = scenario.get('check_coverage')
        if planned is None:
            planned = list(declared_checks)
        else:
            if isinstance(planned, str):
                planned = [planned]
            planned = list(dict.fromkeys(
                [*declared_checks, *(str(item) for item in planned)]))
        self._checks = {
            check: declared_checks.get(check, {}) for check in planned
        }
        self._check_states = {
            check: self._check_states.get(check,
                                          {'status': 'not_run', 'step': None})
            for check in planned
        }
        raw_evidence = scenario.get('failure_evidence', True)
        if raw_evidence is False:
            self._failure_evidence = {}
        elif isinstance(raw_evidence, dict):
            self._failure_evidence = {
                'screenshot': raw_evidence.get('screenshot', True),
                'tree': raw_evidence.get('tree', True),
                'include_layers': raw_evidence.get('include_layers', True),
            }
        else:
            self._failure_evidence = {
                'screenshot': True, 'tree': True, 'include_layers': True,
            }
        self._timeout_seconds = 0.0
        raw_timeout = scenario.get('timeout_seconds', 0)
        try:
            self._timeout_seconds = max(0.0, float(raw_timeout or 0))
        except (TypeError, ValueError):
            failures.append('invalid timeout_seconds: %r' % raw_timeout)
            return name, failures, {}, None, None, 0.0
        started = time.monotonic()
        if self._timeout_seconds > 0:
            self.deadline = started + self._timeout_seconds
        else:
            self.deadline = None
        evidence = None
        steps = scenario.get('steps', [])
        for idx, step in enumerate(steps):
            try:
                self._check_deadline()
                self._step(step)
                self._check_deadline()
            except AssertionError as exc:
                failures.append('step %d (%s): %s' %
                                (idx, step.get('cmd'), exc))
                self._mark_check(step.get('check'), 'fail', idx)
                evidence = self._capture_failure_evidence(idx)
                break
            except Exception as exc:  # transport/setup errors abort scenario
                failures.append('step %d (%s): %s' %
                                (idx, step.get('cmd'), exc))
                self._mark_check(step.get('check'), 'fail', idx)
                evidence = self._capture_failure_evidence(idx)
                break
            else:
                self._mark_check(step.get('check'), 'pass', idx)
        coverage = self._coverage()
        if coverage is not None and not coverage['ok']:
            missing = [check for check, state in self._check_states.items()
                       if state['status'] in ('not_run', 'unplanned', 'fail')]
            if missing:
                failures.append('coverage: %s' % ', '.join(missing))
            if evidence is None:
                evidence = self._capture_failure_evidence(
                    max(0, len(steps) - 1))
        duration = time.monotonic() - started
        return name, failures, self._check_states, coverage, evidence, duration

    def _flybirds_restart_regression(self):
        # Paths retain inherited hidden flags, but deliberately ignore clipping:
        # pipes outside the viewport still determine the next flight target.
        def paths(node, ancestors=(), hidden=False):
            hidden = hidden or node.get('flags', {}).get('hidden', False)
            yield node, ancestors, hidden
            for child in node.get('children', []) + node.get('overlays', []):
                yield from paths(child, ancestors + (node,), hidden)

        def texts(node):
            return [item for item, _, hidden in paths(node)
                    if not hidden and 'text' in item]

        def numeric(node, expected, context):
            numbers = [item['text'] for item in texts(node)
                       if item['text'].isascii() and item['text'].isdecimal()]
            assert numbers == [str(expected)], \
                '%s score %r != %s' % (context, numbers, expected)

        def snapshot():
            entries = list(paths(self.call('sim.tree')['tree']))
            hud_labels = [(node, ancestors) for node, ancestors, hidden in entries
                          if not hidden and node.get('text') == '得分']
            assert len(hud_labels) == 1, 'expected unique visible 得分 HUD'
            _, ancestors = hud_labels[0]
            assert len(ancestors) >= 2, 'HUD lacks field ancestor'
            hud, field = ancestors[-1], ancestors[-2]
            headings = [(node, parents, hidden) for node, parents, hidden in entries
                        if node.get('text') == '本局得分']
            assert len(headings) == 1 and headings[0][1], \
                'expected unique settlement card'
            _, parents, over_hidden = headings[0]
            card = parents[-1]
            assert any(parent is field for parent in parents), \
                'settlement card outside game field'
            return field, hud, card, over_hidden

        def playing(field, over_hidden):
            assert over_hidden, 'premature death before HUD score 1'
            assert not any(node.get('text') in ('轻点屏幕起飞', '已暂停')
                           for node in texts(field)), 'expected PLAYING state'

        def frame():
            self.call('sim.step', {'ms': 33})

        def touch(action, x, y):
            self.call('sim.touch', {'action': action, 'x': x, 'y': y})

        frames = 0
        while True:
            field, hud, card, over_hidden = snapshot()
            playing(field, over_hidden)
            numbers = [node['text'] for node in texts(hud)
                       if node['text'].isascii() and node['text'].isdecimal()]
            assert len(numbers) == 1 and numbers[0] in ('0', '1'), \
                'unexpected HUD score before first point: %r' % numbers
            if numbers == ['1']:
                break
            assert frames < 360, 'HUD score 1 not reached within 360 frames'
            children = [node for node in field.get('children', [])
                        if not node.get('flags', {}).get('hidden', False)]
            box = field['coords']
            birds = [node for node in children
                     if node.get('coords', {}).get('w') == 30 and
                     node['coords'].get('h') == 24 and
                     node['coords'].get('x') == box['x'] + 88]
            assert len(birds) == 1, 'expected unique 30x24 bird at field x88'
            bird = birds[0]['coords']
            pairs = {}
            for node in children:
                coords = node.get('coords', {})
                if coords.get('w') == 60:
                    pairs.setdefault(coords['x'], []).append(coords)
            assert pairs and all(len(pair) == 2 for pair in pairs.values()), \
                'expected upper/lower width60 pipe pairs'
            approaching = [pair for x, pair in pairs.items()
                           if x + 60 >= bird['x']]
            assert approaching, 'no pipe trailing edge ahead of bird'
            pair = min(approaching, key=lambda pair: pair[0]['x'] + 60)
            top, bottom = sorted(pair, key=lambda coords: coords['y'])
            assert top['y'] + top['h'] < bottom['y'], 'invalid pipe gap'
            target = top['y'] + top['h'] + 63 - 12 + 8
            if bird['y'] >= target and frames + 2 <= 360:
                # Stay outside the system's left-edge back-gesture capture.
                # Passive pipe/ground children let this point reach the field.
                x, y = box['x'] + box['w'] // 2, box['y'] + box['h'] - 40
                touch('down', x, y)
                frame()
                frames += 1
                # Even between the two tap frames, inspect the actual tree.
                field, hud, card, over_hidden = snapshot()
                playing(field, over_hidden)
                touch('up', x, y)
                frame()
                frames += 1
            else:
                frame()
                frames += 1

        def wait_over(expected):
            for elapsed in range(121):
                field, hud, card, over_hidden = snapshot()
                numeric(hud, expected, 'HUD')
                if not over_hidden:
                    numeric(card, expected, 'settlement')
                    assert [node['text'] for node in texts(card)
                            if node['text'].startswith('最高 ')] == ['最高 1'], \
                        'settlement highest score must be 1'
                    return card
                assert elapsed < 120, 'settlement not visible within 120 frames'
                frame()

        card = wait_over(1)
        for _ in range(6):
            frame()  # Let the existing 180 ms card fade finish for visual proof.
        self._step({'cmd': 'screenshot', 'name': 'flybirds_over.png'})
        buttons = [node for node in texts(card) if node['text'] == '再来一局']
        assert len(buttons) == 1, 'expected unique settlement restart control'
        box = buttons[0]['coords']
        x, y = box['x'] + box['w'] // 2, box['y'] + box['h'] // 2
        touch('down', x, y)
        frame()
        touch('up', x, y)
        frame()
        field, hud, card, over_hidden = snapshot()
        numeric(hud, 0, 'restart HUD')
        assert over_hidden, 'settlement card must be hidden after restart'
        self._step({'cmd': 'screenshot', 'name': 'flybirds_restart_zero.png'})
        wait_over(0)
        for _ in range(6):
            frame()
        self._step({'cmd': 'screenshot', 'name': 'flybirds_second_over_zero.png'})

    def _step(self, step):
        cmd = step['cmd']
        if cmd == 'flybirds_restart_regression':
            self._flybirds_restart_regression()
            return
        if cmd == 'navigate':
            params = {'app': step['app']}
            if step.get('page'):
                params['page'] = step['page']
            self.call('sim.navigate', params)
        elif cmd == 'step':
            for _ in range(int(step.get('times', 1))):
                self.call('sim.step', {'ms': int(step.get('ms', 33))})
        elif cmd == 'wait_idle':
            result = self.call('sim.wait_idle',
                               {'timeout_ms': step.get('timeout_ms', 5000)})
            assert result.get('idle'), 'not idle: %s' % result
        elif cmd == 'wait_text':
            self._wait_text(step)
        elif cmd == 'wait_tree':
            self._wait_tree(step)
        elif cmd == 'pm':
            result = self.call('sim.pm', {k: step[k] for k in
                                          ('off_ms', 'standby_ms', 'get')
                                          if k in step})
            if step.get('get'):
                want = step.get('pm_state')
                if want is not None:
                    assert result.get('pm_state') == want, \
                        'pm_state %s != %s' % (result.get('pm_state'), want)
        elif cmd == 'sleep_ms':
            seconds = max(0.0, step['ms'] / 1000.0)
            if self.deadline is not None:
                remaining = self.deadline - time.monotonic()
                if remaining <= 0:
                    self._check_deadline()
                seconds = min(seconds, remaining)
            time.sleep(seconds)
            self._check_deadline()
        elif cmd == 'files_root':
            self.call('sim.navigate', {'app': 'files', 'page': 'browse'})
            for _ in range(16):
                self.call('sim.step', {'ms': 660})
                time.sleep(0.05)
                texts = [node.get('text') for node in _walk(self.call('sim.tree')['tree'])]
                if '存储卡 /' in texts:
                    break
                self._step({'cmd': 'tap_text', 'text': '上级'})
            else:
                raise AssertionError('file browser root not reachable')
        elif cmd == 'tap_text':
            nodes = []
            for attempt in range(12):
                tree = self.call('sim.tree')['tree']
                time.sleep(0.05)
                nodes = [target for label, target in
                         _text_targets(tree, step['text'])
                         if label.get('flags', {}).get('visible', True) and
                         64 <= label['coords']['y'] and
                         label['coords']['y'] + label['coords']['h'] <=
                         self.height]
                if nodes:
                    break
                targets = [node for node in _walk(tree) if node.get('text') == step['text']]
                above = targets and targets[-1]['coords']['y'] < 64
                if targets and all(node.get('coords', {}).get('y', 0) >= 64 and
                                   node.get('coords', {}).get('y', 0) +
                                   node.get('coords', {}).get('h', 0) <=
                                   self.height
                                   for node in targets):
                    self.call('sim.step', {'ms': 330})
                    continue
                self._step({'cmd': 'drag', 'x0': 180, 'y0': 170 if above else 390,
                            'x1': 180, 'y1': 390 if above else 170, 'steps': 8})
            assert nodes, 'reachable text not found: %s' % step['text']
            box = nodes[-1]['coords']
            x, y = box['x'] + box['w'] // 2, box['y'] + box['h'] // 2
            self.call('sim.touch', {'action': 'down', 'x': x, 'y': y})
            self.call('sim.step', {'ms': 66})
            self.call('sim.touch', {'action': 'up', 'x': x, 'y': y})
            self.call('sim.step', {'ms': 660})
            time.sleep(0.05)
            self.call('sim.step', {'ms': 330})
        elif cmd == 'long_press_text':
            nodes = []
            for attempt in range(12):
                tree = self.call('sim.tree')['tree']
                time.sleep(0.05)
                nodes = [target for label, target in
                         _text_targets(tree, step['text'])
                         if label.get('flags', {}).get('visible', True) and
                         64 <= label['coords']['y'] and
                         label['coords']['y'] + label['coords']['h'] <=
                         self.height]
                if nodes:
                    break
                targets = [node for node in _walk(tree)
                           if node.get('text') == step['text']]
                above = targets and targets[-1]['coords']['y'] < 64
                if targets and all(node.get('coords', {}).get('y', 0) >= 64 and
                                   node.get('coords', {}).get('y', 0) +
                                   node.get('coords', {}).get('h', 0) <=
                                   self.height
                                   for node in targets):
                    self.call('sim.step', {'ms': 330})
                    continue
                self._step({'cmd': 'drag', 'x0': 180,
                            'y0': 170 if above else 390,
                            'x1': 180, 'y1': 390 if above else 170,
                            'steps': 8})
            assert nodes, 'reachable text not found: %s' % step['text']
            box = nodes[-1]['coords']
            x, y = box['x'] + box['w'] // 2, box['y'] + box['h'] // 2
            self.call('sim.touch', {'action': 'down', 'x': x, 'y': y})
            held = 0
            while held < int(step.get('ms', 700)):
                self.call('sim.step', {'ms': 33})
                held += 33
            self.call('sim.touch', {'action': 'up', 'x': x, 'y': y})
            self.call('sim.step', {'ms': int(step.get('settle_ms', 330))})
        elif cmd == 'touch':
            self.call('sim.touch', {k: step[k] for k in ('action', 'x', 'y')})
        elif cmd == 'touch_pct':
            x = int(round(float(step['x_pct']) * self.width / 100.0))
            y = int(round(float(step['y_pct']) * self.height / 100.0))
            self.call('sim.touch', {'action': step['action'], 'x': x, 'y': y})
        elif cmd == 'touch_bottom':
            if 'x_pct' in step:
                x = int(round(float(step['x_pct']) * self.width / 100.0))
            else:
                x = int(step['x'])
            y = self.height - int(step['y_from_bottom'])
            self.call('sim.touch', {'action': step['action'], 'x': x, 'y': y})
        elif cmd == 'long_press':
            # A long press is a hold, not a click: LVGL reports LONG_PRESSED
            # once long_press_time (400 ms) elapses and still reports CLICKED on
            # release, so the hold must outlast the threshold before lifting.
            self.call('sim.touch', {'action': 'down',
                                    'x': step['x'], 'y': step['y']})
            held = 0
            while held < int(step.get('ms', 700)):
                self.call('sim.step', {'ms': 33})
                held += 33
            self.call('sim.touch', {'action': 'up',
                                    'x': step['x'], 'y': step['y']})
            self.call('sim.step', {'ms': int(step.get('settle_ms', 330))})
        elif cmd == 'drag':
            x0, y0 = int(step['x0']), int(step['y0'])
            x1, y1 = int(step['x1']), int(step['y1'])
            moves = max(int(step.get('steps', 10)), 1)
            pause_ms = int(step.get('ms_per_move', 33))
            self.call('sim.touch', {'action': 'down', 'x': x0, 'y': y0})
            # The press is processed on the next LVGL step, so without a hold
            # the press point is the first move rather than (x0, y0) and
            # press-area assertions silently test the wrong coordinate.
            for _ in range(int(step.get('hold_steps', 0))):
                self.call('sim.step', {'ms': 33})
            for i in range(1, moves + 1):
                self.call('sim.touch', {
                    'action': 'move',
                    'x': x0 + (x1 - x0) * i // moves,
                    'y': y0 + (y1 - y0) * i // moves})
                self.call('sim.step', {'ms': pause_ms})
            self.call('sim.touch', {'action': 'up', 'x': x1, 'y': y1})
            self.call('sim.step', {'ms': int(step.get('settle_ms', 330))})
        elif cmd == 'key':
            self.call('sim.key', {'button': step['button'],
                                  'action': step['action']})
        elif cmd == 'set_power':
            self.call('sim.set_power', {k: step[k] for k in
                                        ('voltage', 'pct', 'charging') if k in step})
        elif cmd == 'set_imu':
            self.call('sim.set_imu', {'pitch': step['pitch'],
                                      'roll': step['roll']})
        elif cmd == 'set_wifi':
            self.call('sim.set_wifi', {'state': step['state']})
        elif cmd == 'set_bluetooth':
            self.call('sim.set_bluetooth',
                      {k: step[k] for k in
                       ('enabled', 'desired_enabled', 'transitioning',
                        'policy_error', 'bound', 'active', 'client_connected',
                        'window_remaining_ms') if k in step})
        elif cmd == 'offer_pairing':
            self.call('sim.offer_pairing',
                      {k: step[k] for k in ('token', 'passkey') if k in step})
        elif cmd == 'set_wifi_profile':
            self.call('sim.set_wifi_profile',
                      {k: step[k] for k in ('ssid', 'password') if k in step})
            # The manager applies the save on its worker; give it a few frames
            # so a following screenshot observes the published profile list.
            for _ in range(6):
                self.call('sim.step', {'ms': 33})
        elif cmd == 'start_wifi_sweep':
            self.call('sim.start_wifi_sweep',
                      {k: step[k] for k in ('ssid',) if k in step})
            # The manager admits the request on its worker and the executor then
            # reports CONNECTING, so a screenshot taken now sees the sweep.
            for _ in range(6):
                self.call('sim.step', {'ms': 33})
        elif cmd == 'set_wifi_scan':
            self.call('sim.set_wifi_scan',
                      {k: step[k] for k in
                       ('records', 'truncated', 'request', 'trigger',
                        'wait_scan', 'status') if k in step})
        elif cmd == 'sd':
            self.call('sim.sd',
                      {k: step[k] for k in
                       ('action', 'name', 'seconds', 'fail', 'start', 'stop') if k in step})
        elif cmd == 'sd_assert':
            result = self.call('sim.sd', {'action': 'list'})
            files = result.get('files', [])
            for name in step.get('contains', []):
                assert name in files, \
                    'sd missing %s in %s' % (name, files)
            for name in step.get('absent', []):
                assert name not in files, \
                    'sd unexpected %s in %s' % (name, files)
        elif cmd == 'nvs':
            self.call('sim.nvs',
                      {k: step[k] for k in
                       ('action', 'key', 'value') if k in step})
        elif cmd == 'music':
            self.call('sim.music',
                      {k: step[k] for k in
                       ('action', 'type', 'mode', 'name', 'path', 'url')
                       if k in step})
        elif cmd == 'set_weather':
            path = step['file']
            if not os.path.isabs(path):
                rooted = os.path.join(MT_ROOT, path)
                if os.path.exists(rooted):
                    path = rooted
            with open(path) as fixture:
                body = fixture.read()
            request = {'endpoint': step['endpoint'], 'body': body,
                       'refresh': step.get('refresh', True)}
            if step.get('status') is not None:
                request['status'] = step['status']
            if step.get('retry_after_seconds') is not None:
                request['retry_after_seconds'] = step['retry_after_seconds']
            self.call('sim.set_weather', request)
        elif cmd == 'set_time':
            self.call('sim.set_time', {'epoch': step['epoch']})
        elif cmd == 'pause':
            self.call('sim.pause', {'enabled': bool(step['enabled'])})
        elif cmd == 'screenshot':
            self.call('sim.screenshot', {'name': step['name'],
                                         'wait_idle': step.get(
                                             'wait_idle', False)})
        elif cmd == 'expect_error':
            reply = self._rpc_reply(step['method'], step.get('params'))
            assert not reply.get('ok'), 'request unexpectedly succeeded: %s' % reply
            assert 'result' not in reply, 'failure returned result: %s' % reply
            want = step.get('error')
            if want is not None:
                assert reply.get('error') == want, \
                    'error %r != %r' % (reply.get('error'), want)
        elif cmd == 'tree_assert':
            present = step.get('contains')
            absent = step.get('absent')
            assert isinstance(present, dict) or isinstance(absent, dict), \
                'tree_assert needs a contains or absent map'
            include_layers = bool((step.get('params') or {}).get(
                'include_layers', False))
            present_result = (self._find_or_none(
                present, include_layers=include_layers, limit=1)
                if present is not None else None)
            absent_result = (self._find_or_none(
                absent, include_layers=include_layers, limit=1)
                if absent is not None else None)
            if ((present is not None and present_result is None) or
                    (absent is not None and absent_result is None)):
                tree = self._tree(include_layers)
                if present is not None:
                    assert any(_flat_matches(node, present)
                               for node in _walk(tree)), \
                        'no node matches %s' % json.dumps(
                            present, ensure_ascii=False)
                if absent is not None:
                    assert not any(_flat_matches(node, absent)
                                   for node in _walk(tree)), \
                        'unexpected node matches %s' % json.dumps(
                            absent, ensure_ascii=False)
            else:
                if present is not None:
                    assert int(present_result.get('total', 0)) > 0, \
                        'no node matches %s' % json.dumps(
                            present, ensure_ascii=False)
                if absent is not None:
                    assert int(absent_result.get('total', 0)) == 0, \
                        'unexpected node matches %s' % json.dumps(
                            absent, ensure_ascii=False)
        elif cmd == 'find_assert':
            query = step.get('query')
            assert isinstance(query, dict) and query, \
                'find_assert needs a non-empty query'
            result = self._find(
                query,
                include_layers=bool(step.get('include_layers', False)),
                limit=int(step.get('limit', 1)),
                visible_only=bool(step.get('visible_only', False)),
            )
            total = int(result.get('total', 0))
            expect = step.get('expect', 'present')
            if expect == 'present':
                assert total > 0, 'no node matches %s' % json.dumps(
                    query, ensure_ascii=False)
            elif expect == 'absent':
                assert total == 0, 'unexpected node matches %s' % json.dumps(
                    query, ensure_ascii=False)
            elif expect == 'count':
                assert total == int(step['count']), \
                    'match count %d != %d' % (total, int(step['count']))
            else:
                raise AssertionError('unknown find_assert expect: %s' % expect)
        elif cmd == 'inspect_assert':
            path = step.get('path')
            assert isinstance(path, list), 'inspect_assert needs a path list'
            result = self.call('sim.inspect', {
                'layer': step.get('layer', 'screen'),
                'path': path,
                'depth': int(step.get('depth', 3)),
                'max_nodes': int(step.get('max_nodes', 256)),
            })
            node = result.get('node') or {}
            present = step.get('contains')
            absent = step.get('absent')
            assert isinstance(present, dict) or isinstance(absent, dict), \
                'inspect_assert needs a contains or absent map'
            if present is not None:
                assert any(_flat_matches(item, present)
                           for item in _walk(node)), \
                    'no inspected node matches %s' % json.dumps(
                        present, ensure_ascii=False)
            if absent is not None:
                assert not any(_flat_matches(item, absent)
                               for item in _walk(node)), \
                    'unexpected inspected node matches %s' % json.dumps(
                        absent, ensure_ascii=False)
        elif cmd == 'hit_test_assert':
            result = self.call('sim.hit_test', {
                'x': int(step['x']), 'y': int(step['y']),
            })
            assert result.get('found'), \
                'no hit-test object at (%d,%d)' % (int(step['x']),
                                                   int(step['y']))
            node = result.get('node') or {}
            contains = step.get('contains')
            if contains is not None:
                assert _flat_matches(node, contains), \
                    'hit-test node does not match %s' % json.dumps(
                        contains, ensure_ascii=False)
        elif cmd == 'call':
            self.call(step['method'], step.get('params'))
        elif cmd == 'assert_result':
            res = self.call(step['method'], step.get('params'))
            for key in (step.get('expect_present') or []):
                assert key in res, 'result missing key %r: %r' % (key, res)
            for key, want in (step.get('expect') or {}).items():
                assert res.get(key) == want, \
                    'result[%s] %r != %r' % (key, res.get(key), want)
            exact = step.get('expect_set')
            if exact is not None:
                key = step.get('field', 'music_pages')
                have = res.get(key)
                assert isinstance(have, list), \
                    'result[%s] is not a list: %r' % (key, have)
                assert set(have) == set(exact), \
                    'result[%s] %r != %r' % (key, have, exact)
            absent = step.get('expect_absent')
            if absent is not None:
                key = step.get('field', 'music_pages')
                have = res.get(key)
                assert isinstance(have, list), \
                    'result[%s] is not a list: %r' % (key, have)
                overlap = sorted(set(have) & set(absent))
                assert not overlap, \
                    'result[%s] unexpectedly contains %r' % (key, overlap)
        else:
            raise RuntimeError('unknown step cmd %r' % cmd)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('scenarios', nargs='+')
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=5002)
    ap.add_argument('--out-dir', default='build/sim/shots')
    ap.add_argument('--report', default=None)
    args = ap.parse_args()

    files = []
    for pattern in args.scenarios:
        files.extend(sorted(glob.glob(pattern)) or [pattern])

    r = Runner(args.host, args.port, args.out_dir)
    r.connect()
    report = {}
    failed = False
    for path in files:
        scenario = json.load(open(path))
        name, failures, checks, coverage, evidence, duration = r.run(scenario)
        report[os.path.basename(path)] = {
            'ok': not failures,
            'failures': failures,
            'checks': checks,
            'coverage': coverage,
            'evidence': evidence,
            'duration_s': round(duration, 3),
        }
        print('%-40s %s' % (os.path.basename(path),
                            'PASS' if not failures else 'FAIL'))
        if coverage is not None:
            print('   checks: %d/%d pass, %d fail, %d not_run, %d unplanned' %
                  (coverage['passed'], coverage['planned'], coverage['failed'],
                   coverage['not_run'], coverage['unplanned']))
        for f in failures:
            print('   -', f)
        failed = failed or bool(failures)
    try:
        if r.sock is None and r._lost_connection:
            r._reconnect()
        r.call('sim.exit', ignore_deadline=True)
    except Exception:
        pass
    r.close()
    if args.report:
        with open(args.report, 'w') as fh:
            json.dump(report, fh, indent=1)
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
