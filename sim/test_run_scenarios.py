"""False-success regressions for the Flybirds scenario's closed-loop action."""
import importlib.util
import json
import os
import socket
import time
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    'run_scenarios', Path(__file__).parent / 'tools/run_scenarios.py')
scenarios = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(scenarios)


def _node(kind, x, y, w, h, *, text=None, hidden=False, children=()):
    node = {'type': kind, 'coords': {'x': x, 'y': y, 'w': w, 'h': h},
            'flags': {'hidden': hidden}, 'children': list(children)}
    if text is not None:
        node['text'] = text
    return node


def _label(text, x, y, w=80, h=24):
    return _node('lv_label', x, y, w, h, text=text)


class FlybirdsRPC:
    """A controlled game surface, advanced only by real 33 ms RPC frames.

    The first run earns one point, then dies without another flap. A geometric
    restart tap starts a zero-point run. Fault modes change observable trees or
    transport replies, not Runner's helper methods or assertions.
    """

    RESTART = (108, 300, 152, 40)

    def __init__(self, fault=None, fail_method=None, fail_occurrence=1):
        self.fault = fault
        self.fail_method = fail_method
        self.fail_occurrence = fail_occurrence
        self.calls = []
        self.method_counts = {}
        self.frames = 0
        self.restart_frames = 0
        self.restarted = False
        self.restart_count = 0
        self.pressed_restart = False
        self.screenshots = []
        self.failed_at = None

    @property
    def first_over(self):
        if self.fault == 'early_death':
            return self.frames >= 2
        return self.fault != 'never_scores' and self.frames >= 6

    @property
    def over(self):
        if self.restarted:
            return self.restart_frames >= 5
        return self.first_over

    @property
    def score(self):
        if self.restarted:
            return {'hidden_hud_zero': '0',
                    'stale_restart_score': '1'}.get(self.fault, '0')
        if self.fault in ('early_death', 'never_scores'):
            return '0'
        return '1' if self.frames >= 4 else '0'

    def tree(self):
        hud_children = [_label('得分', 140, 76)]
        if not (self.restarted and self.fault == 'hidden_hud_zero'):
            hud_children.append(_label(self.score, 210, 76, 24))
        if self.restarted and self.fault == 'hidden_hud_zero':
            # The numeral itself is not hidden: traversal must propagate its
            # ancestor's flag, even when restricting the search to the HUD.
            hud_children.append(_node(
                'lv_obj', 210, 76, 24, 24, hidden=True,
                children=[_label('0', 210, 76, 24)]))
        hud = _node('lv_obj', 132, 70, 112, 40, children=hud_children)
        card_score = ('0' if self.restarted or not self.first_over
                      or self.fault == 'early_death' else '1')
        restart = _node(
            'lv_button', *self.RESTART,
            children=[_label('再来一局', 140, 308, 88)])
        card = _node('lv_obj', 84, 144, 200, 220, hidden=not self.over,
                     children=[_label('本局得分', 132, 166, 104),
                               _label(card_score, 172, 198, 24),
                               _label('最高 1', 144, 242, 88), restart])
        field = _node('lv_obj', 0, 64, 368, 384, children=[
            hud,
            _node('lv_obj', 88, 240, 30, 24),
            # A departed pair remains present but is no longer the target.
            _node('lv_obj', -70, 64, 60, 100),
            _node('lv_obj', -70, 290, 60, 158),
            _node('lv_obj', 180, 64, 60, 100),
            _node('lv_obj', 180, 290, 60, 158),
            card])
        return _node('lv_obj', 0, 0, 368, 448, children=[field])

    def rpc(self, _sock, method, params=None):
        params = params or {}
        self.calls.append((method, dict(params)))
        count = self.method_counts.get(method, 0) + 1
        self.method_counts[method] = count
        if method == self.fail_method and count == self.fail_occurrence:
            self.failed_at = len(self.calls)
            return {'ok': False, 'error': 'injected flybirds RPC failure'}
        if method == 'sim.tree':
            result = {'tree': self.tree()}
        elif method == 'sim.step':
            if params.get('ms') != 33:
                raise AssertionError('fixture requires individual 33 ms frames')
            if self.restarted:
                self.restart_frames += 1
            else:
                self.frames += 1
            result = {}
        elif method == 'sim.touch':
            x, y, w, h = self.RESTART
            inside = (x <= params['x'] < x + w and
                      y <= params['y'] < y + h)
            if params['action'] == 'down':
                self.pressed_restart = self.over and inside
            elif params['action'] == 'up':
                if self.pressed_restart and self.over and inside:
                    self.restarted = True
                    self.restart_frames = 0
                    self.restart_count += 1
                self.pressed_restart = False
            result = {}
        elif method == 'sim.screenshot':
            self.screenshots.append(params['name'])
            result = {'path': params['name']}
        else:
            raise AssertionError('unexpected fixture RPC: %s' % method)
        return {'ok': True, 'result': result}


class TreeAssertTest(unittest.TestCase):
    def setUp(self):
        self.runner = scenarios.Runner('127.0.0.1', 5002, '.')
        self.runner.sock = _AcceptanceSocket()
        self.tree = _node('lv_obj', 0, 0, 368, 448, children=[
            _label('已选择 3/3', 16, 80),
            _label('天气', 16, 120)])

    def _step(self, command):
        with patch.object(scenarios, 'rpc',
                          return_value={'ok': True,
                                        'result': {'tree': self.tree}}):
            self.runner._step(command)

    def test_contains_and_absent_can_be_combined(self):
        self._step({'cmd': 'tree_assert',
                    'contains': {'type': 'lv_label', 'text': '天气'},
                    'absent': {'type': 'lv_label', 'text': '已选满'}})

    def test_absent_rejects_a_matching_node(self):
        with self.assertRaisesRegex(AssertionError, 'unexpected node matches'):
            self._step({'cmd': 'tree_assert',
                        'absent': {'type': 'lv_label', 'text': '已选择 3/3'}})

    def test_contains_requires_a_matching_node(self):
        with self.assertRaisesRegex(AssertionError, 'no node matches'):
            self._step({'cmd': 'tree_assert',
                        'contains': {'type': 'lv_label', 'text': '音乐'}})

    def test_empty_assertion_is_rejected(self):
        with self.assertRaisesRegex(AssertionError,
                                    'needs a contains or absent map'):
            self._step({'cmd': 'tree_assert'})


class KeepGoingTest(unittest.TestCase):
    def _run(self, keep_going, steps, error=None):
        runner = scenarios.Runner(
            '127.0.0.1', 5002, '.', keep_going=keep_going)
        runner._capture_failure_evidence = lambda idx: {'step': idx}
        calls = []

        def fail(step):
            calls.append(step.get('cmd'))
            if error is not None:
                raise error
            raise AssertionError('forced failure')

        runner._step = fail
        result = runner.run({'name': 'keep-going-test', 'steps': steps})
        return result, calls

    def test_default_stops_after_first_assertion(self):
        steps = [{'cmd': 'tree_assert'}, {'cmd': 'find_assert'}]
        result, calls = self._run(False, steps)
        self.assertEqual(result[1], ['step 0 (tree_assert): forced failure'])
        self.assertEqual(calls, ['tree_assert'])

    def test_keep_going_collects_read_only_assertions(self):
        steps = [{'cmd': 'tree_assert'}, {'cmd': 'find_assert'}]
        result, calls = self._run(True, steps)
        self.assertEqual(len(result[1]), 2)
        self.assertEqual(calls, ['tree_assert', 'find_assert'])

    def test_keep_going_stops_before_side_effect_step(self):
        steps = [
            {'cmd': 'tree_assert'},
            {'cmd': 'tap'},
            {'cmd': 'tree_assert'},
        ]
        result, calls = self._run(True, steps)
        self.assertEqual(result[1], [
            'step 0 (tree_assert): forced failure',
            'step 1 (tap): skipped after a failed soft assertion',
        ])
        self.assertEqual(calls, ['tree_assert'])

    def test_keep_going_collects_sd_assertions(self):
        steps = [
            {'cmd': 'tree_assert'},
            {'cmd': 'sd_assert'},
            {'cmd': 'tree_assert'},
        ]
        result, calls = self._run(True, steps)
        self.assertEqual(len(result[1]), 3)
        self.assertEqual(calls, ['tree_assert', 'sd_assert', 'tree_assert'])

    def test_assert_result_is_not_executed_after_soft_failure(self):
        steps = [
            {'cmd': 'tree_assert'},
            {'cmd': 'assert_result', 'method': 'sim.sd'},
            {'cmd': 'tree_assert'},
        ]
        result, calls = self._run(True, steps)
        self.assertEqual(result[1], [
            'step 0 (tree_assert): forced failure',
            'step 1 (assert_result): skipped after a failed soft assertion',
        ])
        self.assertEqual(calls, ['tree_assert'])

    def test_timeout_is_not_a_soft_assertion(self):
        steps = [{'cmd': 'tree_assert'}, {'cmd': 'tree_assert'}]
        result, calls = self._run(
            True, steps, error=scenarios.ScenarioTimeout('deadline'))
        self.assertEqual(len(result[1]), 1)
        self.assertEqual(calls, ['tree_assert'])


class WeatherStepTest(unittest.TestCase):
    def _run(self, step):
        runner = scenarios.Runner('127.0.0.1', 5002, '.')
        runner.sock = _AcceptanceSocket()
        with patch.object(scenarios, 'rpc',
                          return_value={'ok': True, 'result': {}}) as rpc:
            runner._step(step)
        return rpc.call_args.args[2]

    def test_weather_request_preserves_optional_semantics(self):
        with tempfile.NamedTemporaryFile('w', suffix='.json') as fixture:
            fixture.write('{"ok":true}')
            fixture.flush()
            request = self._run({
                'cmd': 'set_weather',
                'endpoint': 'current',
                'file': fixture.name,
                'status': 429,
                'retry_after_seconds': 0,
                'refresh': False,
            })
        self.assertEqual(request['endpoint'], 'current')
        self.assertEqual(request['status'], 429)
        self.assertEqual(request['retry_after_seconds'], 0)
        self.assertIs(request['refresh'], False)

    def test_weather_request_does_not_invent_optional_fields(self):
        with tempfile.NamedTemporaryFile('w', suffix='.json') as fixture:
            fixture.write('{}')
            fixture.flush()
            request = self._run({
                'cmd': 'set_weather',
                'endpoint': 'daily',
                'file': fixture.name,
            })
        self.assertNotIn('status', request)
        self.assertNotIn('retry_after_seconds', request)
        self.assertIs(request['refresh'], True)


class FlybirdsRestartRegressionTest(unittest.TestCase):
    def _run(self, surface):
        runner = scenarios.Runner('127.0.0.1', 5002, '.')
        runner.sock = _AcceptanceSocket()
        with patch.object(scenarios, 'rpc', side_effect=surface.rpc):
            runner._step({'cmd': 'flybirds_restart_regression'})

    def test_hidden_zero_under_hud_cannot_satisfy_restart_score(self):
        surface = FlybirdsRPC('hidden_hud_zero')
        with self.assertRaisesRegex(AssertionError, r'(?i)hud|score|得分'):
            self._run(surface)
        self.assertEqual(surface.restart_count, 1)
        self.assertEqual(surface.score, '0')
        self.assertLess(surface.restart_frames, 5,
                        'the incorrect restart HUD must fail before death')
        self.assertNotIn('flybirds_second_over_zero.png', surface.screenshots)

    def test_death_before_one_point_aborts_without_restart(self):
        surface = FlybirdsRPC('early_death')
        with self.assertRaisesRegex(AssertionError, r'(?i)died|death|over|死亡|结算'):
            self._run(surface)
        self.assertEqual(surface.frames, 2)
        self.assertEqual(surface.restart_count, 0)
        self.assertEqual(surface.screenshots, [])

    def test_scoring_deadline_counts_actual_frames_not_flaps(self):
        surface = FlybirdsRPC('never_scores')
        with self.assertRaisesRegex(AssertionError, r'(?i)360|timeout|score|得分'):
            self._run(surface)
        self.assertEqual(surface.frames, 360)
        self.assertEqual(surface.restart_count, 0)
        self.assertEqual(surface.screenshots, [])

    def test_failed_rpc_aborts_at_the_failing_request(self):
        for method, occurrence in [('sim.tree', 2), ('sim.touch', 1),
                                   ('sim.step', 1), ('sim.screenshot', 1)]:
            with self.subTest(method=method):
                surface = FlybirdsRPC(fail_method=method,
                                      fail_occurrence=occurrence)
                with self.assertRaisesRegex(
                        RuntimeError, 'injected flybirds RPC failure'):
                    self._run(surface)
                self.assertIsNotNone(surface.failed_at)
                self.assertEqual(len(surface.calls), surface.failed_at,
                                 'no RPC may follow a failed request')

    def test_restart_with_stale_one_is_not_hidden_settlement_zero(self):
        surface = FlybirdsRPC('stale_restart_score')
        with self.assertRaisesRegex(AssertionError, r'(?i)hud|score|得分'):
            self._run(surface)
        self.assertEqual(surface.restart_count, 1)
        self.assertEqual(surface.score, '1')
        self.assertLess(surface.restart_frames, 5,
                        'stale HUD must be rejected immediately after restart')
        self.assertNotIn('flybirds_second_over_zero.png', surface.screenshots)


class _AcceptanceSocket:
    def __init__(self):
        self.timeout = 60.0

    def settimeout(self, value):
        self.timeout = value

    def gettimeout(self):
        return self.timeout

    def close(self):
        pass


class AcceptanceRPC:
    """Small deterministic RPC surface for the scenario acceptance contract."""

    def __init__(self, text='时钟'):
        self.frames = 0
        self.text = text
        self.screenshots = []
        self.calls = []

    def tree(self):
        return _node('lv_obj', 0, 0, 368, 448, children=[
            _label(self.text if self.frames else '准备', 16, 80),
        ])

    def rpc(self, _sock, method, params=None):
        self.calls.append((method, dict(params or {})))
        if method == 'sim.ping':
            return {'ok': True, 'result': {'width': 368, 'height': 448}}
        if method == 'sim.tree':
            return {'ok': True, 'result': {'tree': self.tree()}}
        if method == 'sim.find':
            query = params.get('query') or {}
            found = (self.frames > 0 and
                     query.get('text') == self.text)
            return {'ok': True, 'result': {
                'matches': [], 'total': 1 if found else 0,
                'truncated': False,
            }}
        if method == 'sim.inspect':
            return {'ok': True, 'result': {'node': self.tree()}}
        if method == 'sim.step':
            self.frames += 1
            return {'ok': True, 'result': {}}
        if method == 'sim.screenshot':
            self.screenshots.append(params['name'])
            return {'ok': True, 'result': {'path': params['name']}}
        if method == 'sim.wait_idle':
            return {'ok': True, 'result': {'idle': True}}
        raise AssertionError('unexpected RPC: %s' % method)


class ScenarioAcceptanceTest(unittest.TestCase):
    def _run(self, scenario, fake=None):
        fake = fake or AcceptanceRPC()
        runner = scenarios.Runner('127.0.0.1', 5002, tempfile.mkdtemp())
        runner.sock = _AcceptanceSocket()
        with patch.object(scenarios, 'rpc', side_effect=fake.rpc):
            return runner.run(scenario), fake

    def test_checks_pass_and_coverage_is_complete(self):
        result, _ = self._run({
            'name': 'accept',
            'checks': {'visible': 'clock visible'},
            'steps': [{'cmd': 'wait_text', 'text': '时钟',
                       'timeout_ms': 500, 'check': 'visible'}],
        })
        _, failures, checks, coverage, _, _ = result
        self.assertEqual(failures, [])
        self.assertEqual(checks['visible']['status'], 'pass')
        self.assertTrue(coverage['ok'])
        self.assertEqual(coverage['passed'], 1)

    def test_missing_coverage_check_fails(self):
        result, _ = self._run({
            'name': 'accept',
            'checks': {'visible': 'clock visible', 'absent': 'no error'},
            'steps': [{'cmd': 'wait_text', 'text': '时钟',
                       'timeout_ms': 500, 'check': 'visible'}],
        })
        _, failures, checks, coverage, _, _ = result
        self.assertEqual(checks['absent']['status'], 'not_run')
        self.assertFalse(coverage['ok'])
        self.assertTrue(any('coverage:' in item for item in failures))

    def test_unplanned_step_check_fails(self):
        result, _ = self._run({
            'name': 'accept',
            'checks': {'visible': 'clock visible'},
            'steps': [{'cmd': 'wait_text', 'text': '时钟',
                       'timeout_ms': 500, 'check': 'unexpected'}],
        })
        _, failures, checks, coverage, _, _ = result
        self.assertEqual(checks['unexpected']['status'], 'unplanned')
        self.assertFalse(coverage['ok'])
        self.assertTrue(any('coverage:' in item for item in failures))

    def test_repeated_unplanned_check_stays_unplanned(self):
        result, _ = self._run({
            'name': 'accept',
            'checks': {'visible': 'clock visible'},
            'steps': [
                {'cmd': 'wait_text', 'text': '时钟',
                 'timeout_ms': 500, 'check': 'visible'},
                {'cmd': 'wait_text', 'text': '时钟',
                 'timeout_ms': 500, 'check': 'unexpected'},
                {'cmd': 'wait_text', 'text': '时钟',
                 'timeout_ms': 500, 'check': 'unexpected'},
            ],
        })
        _, failures, checks, coverage, _, _ = result
        self.assertEqual(checks['unexpected']['status'], 'unplanned')
        self.assertFalse(coverage['ok'])
        self.assertTrue(any('coverage:' in item for item in failures))

    def test_step_check_without_declared_checks_is_unplanned(self):
        result, _ = self._run({
            'name': 'accept',
            'steps': [{'cmd': 'wait_text', 'text': '时钟',
                       'timeout_ms': 500, 'check': 'unexpected'}],
        })
        _, failures, checks, coverage, _, _ = result
        self.assertEqual(checks['unexpected']['status'], 'unplanned')
        self.assertIsNotNone(coverage)
        self.assertFalse(coverage['ok'])
        self.assertTrue(any('coverage:' in item for item in failures))

    def test_failure_evidence_writes_screenshot_and_tree(self):
        tmp = tempfile.mkdtemp()
        fake = AcceptanceRPC(text='其他')
        runner = scenarios.Runner('127.0.0.1', 5002, tmp)
        runner.sock = _AcceptanceSocket()
        scenario = {
            'name': 'accept',
            'failure_evidence': {'screenshot': True, 'tree': True},
            'steps': [{'cmd': 'tree_assert',
                       'contains': {'type': 'lv_label', 'text': '不存在'}}],
        }
        with patch.object(scenarios, 'rpc', side_effect=fake.rpc):
            _, failures, _, _, evidence, _ = runner.run(scenario)
        self.assertTrue(failures)
        self.assertIsNotNone(evidence)
        self.assertTrue(os.path.isfile(os.path.join(
            tmp, 'failure_accept_step_0.tree.json')))
        self.assertEqual(evidence['screenshot'], 'failure_accept_step_0.png')

    def test_scenario_timeout_is_a_failure(self):
        fake = AcceptanceRPC(text='其他')
        result, _ = self._run({
            'name': 'accept',
            'timeout_seconds': 0.001,
            'steps': [{'cmd': 'wait_text', 'text': '时钟',
                       'timeout_ms': 2000}],
        }, fake)
        _, failures, _, _, _, _ = result
        self.assertTrue(any('timeout' in item.lower() for item in failures))

    def test_sleep_step_cannot_exceed_scenario_timeout(self):
        runner = scenarios.Runner('127.0.0.1', 5002, tempfile.mkdtemp())
        runner.sock = _AcceptanceSocket()
        started = time.monotonic()
        _, failures, _, _, _, _ = runner.run({
            'name': 'sleep',
            'timeout_seconds': 0.05,
            'steps': [{'cmd': 'sleep_ms', 'ms': 500}],
        })
        self.assertTrue(any('timeout' in item.lower() for item in failures))
        self.assertLess(time.monotonic() - started, 1.0)

    def test_expect_error_honors_scenario_deadline(self):
        runner = scenarios.Runner('127.0.0.1', 5002, '.')
        runner.sock = _AcceptanceSocket()
        runner._timeout_seconds = 1.0
        runner.deadline = time.monotonic() - 1
        with self.assertRaises(scenarios.ScenarioTimeout):
            runner._step({'cmd': 'expect_error', 'method': 'sim.find',
                          'params': {'query': {}}, 'error': 'empty query'})

    def test_coverage_failure_captures_evidence(self):
        tmp = tempfile.mkdtemp()
        fake = AcceptanceRPC(text='时钟')
        fake.frames = 1
        runner = scenarios.Runner('127.0.0.1', 5002, tmp)
        runner.sock = _AcceptanceSocket()
        scenario = {
            'name': 'coverage',
            'checks': {'visible': 'clock visible'},
            'steps': [{'cmd': 'wait_text', 'text': '时钟',
                       'timeout_ms': 500}],
        }
        with patch.object(scenarios, 'rpc', side_effect=fake.rpc):
            _, failures, _, coverage, evidence, _ = runner.run(scenario)
        self.assertTrue(any('coverage:' in item for item in failures))
        self.assertFalse(coverage['ok'])
        self.assertIsNotNone(evidence)
        self.assertTrue(os.path.isfile(os.path.join(
            tmp, 'failure_coverage_step_0.tree.json')))


class ScenarioValidationTest(unittest.TestCase):
    def test_invalid_timeout_is_reported(self):
        runner = scenarios.Runner('127.0.0.1', 5002, '.')
        runner.sock = _AcceptanceSocket()
        result = runner.run({'name': 'bad', 'timeout_seconds': 'soon',
                             'steps': []})
        _, failures, _, _, _, _ = result
        self.assertTrue(any('invalid timeout_seconds' in item
                            for item in failures))

    def test_wait_text_rejects_nonpositive_timeout(self):
        runner = scenarios.Runner('127.0.0.1', 5002, '.')
        runner.sock = _AcceptanceSocket()
        with self.assertRaisesRegex(AssertionError,
                                    'timeout_ms must be greater than zero'):
            runner._wait_text({'text': '时钟', 'timeout_ms': 0})

    def test_wait_tree_rejects_nonpositive_timeout(self):
        runner = scenarios.Runner('127.0.0.1', 5002, '.')
        runner.sock = _AcceptanceSocket()
        with self.assertRaisesRegex(AssertionError,
                                    'timeout_ms must be greater than zero'):
            runner._wait_tree({'contains': {'type': 'lv_label'},
                               'timeout_ms': 0})

    def test_wait_text_recomputes_budget_before_tree_fallback(self):
        runner = scenarios.Runner('127.0.0.1', 5002, '.')
        runner.sock = _AcceptanceSocket()
        tree_calls = []

        def slow_find(_query, include_layers=False, limit=1,
                      visible_only=False, timeout=None):
            time.sleep(0.06)
            return None

        runner._find_or_none = slow_find
        runner._tree = lambda *_args, **_kwargs: tree_calls.append(True)
        with self.assertRaisesRegex(AssertionError, 'wait_text timeout'):
            runner._wait_text({'text': 'never', 'timeout_ms': 50,
                               'interval_ms': 1})
        self.assertEqual(tree_calls, [])

    def test_wait_passes_remaining_budget_to_every_rpc(self):
        runner = scenarios.Runner('127.0.0.1', 5002, '.')
        runner.sock = _AcceptanceSocket()
        find_timeouts = []
        step_timeouts = []

        def fake_find(_query, include_layers=False, limit=1,
                      visible_only=False, timeout=None):
            find_timeouts.append(timeout)
            return {'total': 0, 'matches': [], 'truncated': False}

        def fake_call(method, params=None, timeout=None):
            step_timeouts.append(timeout)
            return {}

        runner._find_or_none = fake_find
        runner._tree = lambda *_args, **_kwargs: _node(
            'lv_obj', 0, 0, 368, 448)
        runner.call = fake_call
        with self.assertRaisesRegex(AssertionError, 'wait_text timeout'):
            runner._wait_text({'text': 'never', 'timeout_ms': 150,
                               'interval_ms': 1})
        self.assertTrue(find_timeouts)
        self.assertTrue(step_timeouts)
        self.assertTrue(all(value is not None and value <= 0.15
                            for value in find_timeouts + step_timeouts))


class QueryStepTest(unittest.TestCase):
    def _runner(self):
        runner = scenarios.Runner('127.0.0.1', 5002, '.')
        runner.sock = _AcceptanceSocket()
        return runner

    def test_find_assert_present_and_absent(self):
        runner = self._runner()
        fake = AcceptanceRPC(text='时钟')
        fake.frames = 1
        with patch.object(scenarios, 'rpc', side_effect=fake.rpc):
            runner._step({'cmd': 'find_assert',
                          'query': {'text': '时钟'},
                          'expect': 'present'})
            runner._step({'cmd': 'find_assert',
                          'query': {'text': '不存在'},
                          'expect': 'absent'})

    def test_inspect_assert_checks_subtree(self):
        runner = self._runner()
        fake = AcceptanceRPC(text='时钟')
        with patch.object(scenarios, 'rpc', side_effect=fake.rpc):
            runner._step({'cmd': 'inspect_assert', 'layer': 'screen',
                          'path': [], 'depth': 2,
                          'contains': {'type': 'lv_obj'}})

    def test_tree_assert_uses_find_when_supported(self):
        runner = self._runner()
        fake = AcceptanceRPC(text='时钟')
        fake.frames = 1
        with patch.object(scenarios, 'rpc', side_effect=fake.rpc):
            runner._step({'cmd': 'tree_assert',
                          'contains': {'type': 'lv_label',
                                       'text': '时钟'}})
        self.assertIn(('sim.find', {'query': {'type': 'lv_label',
                                               'text': '时钟'},
                                    'include_layers': False,
                                    'limit': 1,
                                    'visible_only': False}),
                      fake.calls)
        self.assertNotIn('sim.tree', [call[0] for call in fake.calls])


class RpcTimeoutSocketTest(unittest.TestCase):
    def test_timeout_drops_socket_for_reconnect(self):
        class LostSocket:
            def __init__(self):
                self.closed = False
            def gettimeout(self):
                return 60.0
            def settimeout(self, _value):
                pass
            def close(self):
                self.closed = True

        runner = scenarios.Runner('127.0.0.1', 5002, '.')
        sock = LostSocket()
        runner.sock = sock
        with patch.object(scenarios, 'rpc', side_effect=socket.timeout('late')):
            with self.assertRaises(socket.timeout):
                runner._rpc_reply('sim.wait_idle')
        self.assertTrue(sock.closed)
        self.assertIsNone(runner.sock)
        self.assertTrue(runner._lost_connection)

    def test_missing_socket_fails_without_calling_rpc(self):
        runner = scenarios.Runner('127.0.0.1', 5002, '.')
        with patch.object(scenarios, 'rpc') as rpc:
            with self.assertRaisesRegex(RuntimeError,
                                        'connection is not established'):
                runner._rpc_reply('sim.ping')
        rpc.assert_not_called()
        self.assertTrue(runner._lost_connection)

    def test_reconnect_inherits_rpc_budget(self):
        runner = scenarios.Runner('127.0.0.1', 5002, '.')
        sock = _AcceptanceSocket()
        seen = []

        def fake_reconnect(timeout=None):
            seen.append(timeout)
            runner.sock = sock
            runner._lost_connection = False

        runner._lost_connection = True
        runner._reconnect = fake_reconnect
        with patch.object(scenarios, 'rpc',
                          return_value={'ok': True, 'result': {}}):
            runner._rpc_reply('sim.ping', timeout=0.25)
        self.assertEqual(seen, [0.25])


if __name__ == '__main__':
    unittest.main()
