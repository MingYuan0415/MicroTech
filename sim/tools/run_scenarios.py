#!/usr/bin/env python3
"""Scenario runner for the MicroTech LVGL simulator.

Scenario JSON:
  {
    "name": "...",
    "steps": [
      {"cmd": "navigate", "app": "clock"},
      {"cmd": "step", "ms": 33, "times": 20},
      {"cmd": "wait_idle", "timeout_ms": 4000},
      {"cmd": "set_power", "voltage": 3900, "pct": 66, "charging": false},
      {"cmd": "screenshot", "name": "clock.png"},
      {"cmd": "tree_assert", "contains": {"type": "lv_label", "text": "时钟"}},
      {"cmd": "touch", "action": "down", "x": 20, "y": 30}
    ]
  }

tree_assert: every node matching ALL given key/values must exist
(coords/flags/state are checked via the flattened dotted key, e.g.
 "coords.x" style nesting is supported by walking dicts).

Usage:
  python3 sim/tools/run_scenarios.py sim/ci/scenarios/*.json [--golden DIR]
      [--update] [--report out.json] [--host H] [--port P]
"""
import argparse
import glob
import hashlib
import json
import os
import socket
import subprocess
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
        if not ok or cur != want:
            return False
    return True


def _walk(node):
    yield node
    for child in node.get('children', []):
        yield from _walk(child)


class Runner:
    def __init__(self, host, port, out_dir, golden, update):
        self.host, self.port = host, port
        self.out_dir, self.golden, self.update = out_dir, golden, update
        self.sock = None

    def connect(self):
        self.sock = socket.create_connection((self.host, self.port), timeout=60)

    def call(self, method, params=None):
        reply = rpc(self.sock, method, params)
        if not reply.get('ok'):
            raise RuntimeError('%s failed: %s' % (method, reply))
        return reply['result']

    def close(self):
        if self.sock:
            self.sock.close()
            self.sock = None

    def run(self, scenario):
        failures = []
        name = scenario.get('name', 'scenario')
        for idx, step in enumerate(scenario.get('steps', [])):
            try:
                self._step(step)
            except AssertionError as exc:
                failures.append('step %d (%s): %s' % (idx, step.get('cmd'), exc))
                break
            except Exception as exc:  # transport/setup errors abort scenario
                failures.append('step %d (%s): %s' % (idx, step.get('cmd'), exc))
                break
        return name, failures

    def _step(self, step):
        cmd = step['cmd']
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
            time.sleep(step['ms'] / 1000.0)
        elif cmd == 'touch':
            self.call('sim.touch', {k: step[k] for k in ('action', 'x', 'y')})
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
        elif cmd == 'set_weather':
            path = step['file']
            if not os.path.isabs(path):
                rooted = os.path.join(MT_ROOT, path)
                if os.path.exists(rooted):
                    path = rooted
            body = open(path).read()
            self.call('sim.set_weather', {'endpoint': step['endpoint'],
                                          'status': 200, 'body': body})
        elif cmd == 'set_time':
            self.call('sim.set_time', {'epoch': step['epoch']})
        elif cmd == 'screenshot':
            path = self.call('sim.screenshot', {'name': step['name'],
                                                'wait_idle': False})['path']
            if self.golden:
                golden_file = os.path.join(self.golden, step['name'])
                digest = hashlib.sha256(open(path, 'rb').read()).hexdigest()
                if self.update:
                    import shutil
                    os.makedirs(self.golden, exist_ok=True)
                    shutil.copyfile(path, golden_file)
                elif not os.path.exists(golden_file):
                    # PNG 辅门禁挂起期（固件 GUI 未定标）：缺基线只提示不失败。
                    print('   note: golden %s 未生成，PNG 比对跳过'
                          % step['name'])
                else:
                    gd = hashlib.sha256(open(golden_file, 'rb').read()).hexdigest()
                    assert gd == digest, 'png mismatch %s != %s' % (gd[:12], digest[:12])
        elif cmd == 'tree_assert':
            tree = self.call('sim.tree')['tree']
            match = step.get('contains')
            assert isinstance(match, dict), 'tree_assert needs a contains map'
            any_match = any(_flat_matches(node, match)
                            for node in _walk(tree))
            assert any_match, 'no node matches %s' % json.dumps(match, ensure_ascii=False)
        else:
            raise RuntimeError('unknown step cmd %r' % cmd)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('scenarios', nargs='+')
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=5002)
    ap.add_argument('--out-dir', default='build/sim/shots')
    ap.add_argument('--golden', default=None)
    ap.add_argument('--update', action='store_true')
    ap.add_argument('--report', default=None)
    args = ap.parse_args()

    files = []
    for pattern in args.scenarios:
        files.extend(sorted(glob.glob(pattern)) or [pattern])

    r = Runner(args.host, args.port, args.out_dir, args.golden, args.update)
    r.connect()
    report = {}
    failed = False
    for path in files:
        scenario = json.load(open(path))
        name, failures = r.run(scenario)
        report[os.path.basename(path)] = {'ok': not failures,
                                          'failures': failures}
        print('%-40s %s' % (os.path.basename(path),
                            'PASS' if not failures else 'FAIL'))
        for f in failures:
            print('   -', f)
        failed = failed or bool(failures)
    try:
        r.call('sim.exit')
    except Exception:
        pass
    r.close()
    if args.report:
        with open(args.report, 'w') as fh:
            json.dump(report, fh, indent=1)
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
