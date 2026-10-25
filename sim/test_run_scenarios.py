"""False-success regressions for the Flybirds scenario's closed-loop action."""
import importlib.util
from pathlib import Path
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


class FlybirdsRestartRegressionTest(unittest.TestCase):
    def _run(self, surface):
        runner = scenarios.Runner('127.0.0.1', 5002, '.', None, False)
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


if __name__ == '__main__':
    unittest.main()
