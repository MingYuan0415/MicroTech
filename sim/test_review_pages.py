"""Regressions for false-success cases in simulator review tooling."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import Mock

SPEC = importlib.util.spec_from_file_location(
    'review_pages', Path(__file__).parent / 'tools/review_pages.py')
review = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(review)


class ReviewPagesTest(unittest.TestCase):
    def test_selection_must_include_executable_states(self):
        spec = {'pages': [{'app': 'settings', 'page': 'wifi',
                           'states': [{'name': 'idle'}]}]}
        self.assertEqual(len(review._selected_pages(spec, 'settings/wifi')), 1)
        with self.assertRaises(ValueError):
            review._selected_pages(spec, 'settings/wiif')
        spec['pages'][0]['states'][0]['hardware_only'] = True
        with self.assertRaises(ValueError):
            review._selected_pages(spec, None)

    def test_wait_idle_checks_payload(self):
        runner = Mock()
        runner.call.return_value = {'idle': False}
        with self.assertRaises(RuntimeError):
            review._wait_idle(runner)
        runner.call.return_value = {'idle': True}
        review._wait_idle(runner)

    def test_scroll_requires_direction_and_sufficient_travel(self):
        button = {'type': 'lv_button', 'coords': {'x': 10, 'y': -40, 'w': 60, 'h': 30}}
        owner = {'coords': {'x': 0, 'y': 0, 'w': 368, 'h': 448},
                 'flags': {'scrollable': True}, 'children': [button]}
        self.assertTrue(review._lint(owner, {}, {}, 368, 448))
        button['coords']['y'] = 480
        owner['scroll'] = {'dir': 12, 'bottom': 100}
        self.assertFalse(review._lint(owner, {}, {}, 368, 448))
        button['coords']['x'] = 380
        self.assertTrue(review._lint(owner, {}, {}, 368, 448))
        button['coords']['x'] = 10
        owner['scroll']['bottom'] = 10
        self.assertTrue(review._lint(owner, {}, {}, 368, 448))
        owner['scroll'] = {'dir': 3, 'bottom': 100}
        self.assertTrue(review._lint(owner, {}, {}, 368, 448))
        owner['flags']['hidden'] = True
        self.assertFalse(review._lint(owner, {}, {}, 368, 448))
