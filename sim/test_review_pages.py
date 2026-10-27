"""Regressions for false-success cases in simulator review tooling."""
import importlib.util
import os
import tempfile
from pathlib import Path
import unittest
from unittest.mock import Mock

from PIL import Image

SPEC = importlib.util.spec_from_file_location(
    'review_pages', Path(__file__).parent / 'tools/review_pages.py')
review = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(review)

BOX_Y, BOX_H, VH = 64, 384, 448
CHROME = (0, 0, 200)


def _content_color(row):
    return (row % 256, row // 256, 7)


class FakePageRunner:
    """Agent whose frames encode which page-content row each panel row shows.

    Row `y` of the panel carries the colour of content row
    `offset + y - BOX_Y`, and rows above the scroller carry CHROME. A stitched
    canvas is then checkable row by row without any simulator.
    """

    def __init__(self, travel, stuck=False, initial_offset=0, advance=None):
        self.travel = travel
        self.stuck = stuck
        self.offset = initial_offset
        self.advance = advance
        self.press_y = None
        self.frames = 0
        self.dir = tempfile.TemporaryDirectory()

    def _frame(self):
        image = Image.new('RGB', (368, VH))
        pixels = image.load()
        for y in range(VH):
            color = CHROME if y < BOX_Y else _content_color(
                self.offset + y - BOX_Y)
            for x in range(4):
                pixels[x, y] = color
        path = os.path.join(self.dir.name, 'f%d.png' % self.frames)
        self.frames += 1
        image.save(path)
        return path

    def call(self, method, params=None):
        if method == 'sim.tree':
            return {'tree': {
                'type': 'lv_obj',
                'coords': {'x': 0, 'y': BOX_Y, 'w': 368, 'h': BOX_H},
                'flags': {'scrollable': True, 'hidden': False},
                'scroll': {'dir': 12, 'top': self.offset,
                           'bottom': max(0, self.travel - self.offset),
                           'y': self.offset}}}
        if method == 'sim.screenshot':
            return {'path': self._frame()}
        if method == 'sim.wait_idle':
            return {'idle': True}
        if method == 'sim.step':
            return {}
        if method == 'sim.touch':
            action = (params or {}).get('action')
            if action == 'down':
                self.press_y = params['y']
            elif action == 'up' and not self.stuck:
                step = (self.advance if self.advance is not None
                        else self.press_y - params['y'])
                # The content follows the finger, clamped like scroll_by_bounded.
                self.offset = max(0, min(self.travel, self.offset + step))
            return {}
        raise AssertionError(method)


class ReviewPagesTest(unittest.TestCase):
    def _fake_page(self, travel, **kwargs):
        runner = FakePageRunner(travel, **kwargs)
        self.addCleanup(runner.dir.cleanup)
        return runner

    def test_selection_must_include_executable_states(self):
        spec = {'pages': [{'app': 'settings', 'page': 'wifi',
                           'states': [{'name': 'idle'}]}]}
        self.assertEqual(len(review._selected_pages(spec, 'settings/wifi')), 1)
        with self.assertRaises(ValueError):
            review._selected_pages(spec, 'settings/wiif')
        spec['pages'][0]['states'][0]['hardware_only'] = True
        with self.assertRaises(ValueError):
            review._selected_pages(spec, None)

    def test_busy_port_is_never_silently_reused(self):
        """A second run must not attach to a sim another run is driving.

        Attaching skips per-page isolation and the fixture seed, so captures
        describe whatever the other session navigated to and the failures move
        between runs. Explicit --no-launch stays the one way to drive an
        external session.
        """
        self.assertTrue(review._session_ownership(False, False, 5002))
        self.assertFalse(review._session_ownership(True, False, 5002))
        self.assertFalse(review._session_ownership(True, True, 5002))
        with self.assertRaises(ValueError) as caught:
            review._session_ownership(False, True, 5002)
        self.assertIn('5002', str(caught.exception))
        self.assertIn('--no-launch', str(caught.exception))

    def test_wait_idle_checks_payload(self):
        runner = Mock()
        runner.call.return_value = {'idle': False}
        with self.assertRaises(RuntimeError):
            review._wait_idle(runner)
        runner.call.return_value = {'idle': True}
        review._wait_idle(runner)

    def test_forbidden_text_detects_false_success_and_stale_rows(self):
        label = self._label('已删除', ['已删除'])
        state = {'expect_absent': ['已删除', '旧目录文件']}
        self.assertEqual(review._lint(label, {}, state, 368, 448),
                         ["unexpected text '已删除'"])
        label = self._label('删除失败', ['删除失败'])
        self.assertEqual(review._lint(label, {}, state, 368, 448), [])

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

    def test_folded_in_content_is_still_linted(self):
        """`flags.visible` is false below the fold; the lint must not skip it."""
        label = {'type': 'lv_label',
                 'coords': {'x': 12, 'y': 420, 'w': 300, 'h': 60},
                 'flags': {'visible': False},
                 'styles': {'line_height': 24}, 'content_h': 60,
                 'lines': [{'text': '恢复出厂', 'w': 200},
                           {'text': '设置', 'w': 48}],
                 'lines_complete': True, 'text': '恢复出厂设置'}
        tree = {'type': 'lv_obj', 'coords': {'x': 0, 'y': 64, 'w': 368, 'h': 384},
                'flags': {'scrollable': True, 'visible': True},
                'scroll': {'dir': 12, 'bottom': 226, 'y': 0},
                'children': [label]}
        self.assertEqual(review._visible_labels(tree), [label])
        violations = review._lint(tree, {}, {'single_line': ['恢复出厂设置']},
                                  368, 448)
        self.assertTrue(any('wrapped single-line' in v for v in violations))

    def test_scroll_owner_prefers_page_over_nested_list(self):
        page = {'type': 'lv_obj', 'coords': {'x': 0, 'y': 64, 'w': 368, 'h': 384},
                'flags': {'scrollable': True},
                'scroll': {'dir': 12, 'bottom': 474, 'y': 0}}
        nested = {'type': 'lv_obj', 'coords': {'x': 18, 'y': 74, 'w': 332, 'h': 259},
                  'flags': {'scrollable': True},
                  'scroll': {'dir': 12, 'bottom': 245, 'y': 0}}
        page['children'] = [nested]
        owner = review._scroll_owner(page, 368, 448)
        self.assertEqual(owner[0]['w'], 368)
        self.assertEqual(owner[1], 0)
        self.assertEqual(owner[2], 474)

    def test_stitched_canvas_maps_every_row_to_its_page_content(self):
        """Pins the row mapping: canvas row R shows content row R - box.y, and
        the rows above the scroller keep the first frame's chrome."""
        for travel in (72, 226, 527):
            with self.subTest(travel=travel):
                runner = self._fake_page(travel)
                path, note = review._capture_full_page(runner, 'page', 368, VH)
                with Image.open(path) as handle:
                    image = handle.convert('RGB')
                self.assertEqual(image.size, (368, VH + travel))
                self.assertIn('page content %d px' % (BOX_H + travel), note)
                pixels = image.load()
                for y in range(BOX_Y):
                    self.assertEqual(pixels[0, y], CHROME,
                                     'row %d lost the page chrome' % y)
                for row in range(BOX_H + travel):
                    self.assertEqual(
                        pixels[0, BOX_Y + row], _content_color(row),
                        'canvas row %d must show content row %d'
                        % (BOX_Y + row, row))

    def test_tail_shorter_than_one_scroll_step_stays_a_single_frame(self):
        """A tail below LVGL's drag limit is unreachable, so the faithful
        artifact is the plain frame, not a composite with a missing strip."""
        runner = self._fake_page(review.MIN_REACHABLE_TRAVEL)
        path, note = review._capture_full_page(runner, 'page', 368, VH)
        with Image.open(path) as handle:
            self.assertEqual(handle.size, (368, VH))
        self.assertIsNone(note)

    def test_modal_scrim_preserves_one_surface_frame(self):
        runner = self._fake_page(200, stuck=True)
        original_call = runner.call

        def modal_call(method, params=None):
            result = original_call(method, params)
            if method == 'sim.tree':
                result['tree']['children'] = [{
                    'coords': {'x': 0, 'y': 0, 'w': 368, 'h': VH},
                    'flags': {'floating': True, 'clickable': True}}]
            return result

        runner.call = modal_call
        path, note = review._capture_full_page(runner, 'modal', 368, VH)
        with Image.open(path) as image:
            self.assertEqual(image.size, (368, VH))
            self.assertEqual(image.getpixel((0, VH - 1)), _content_color(VH - 1 - BOX_Y))
        self.assertIn('modal', note)
        self.assertEqual(runner.offset, 0)

    def test_unscrollable_page_never_yields_a_short_canvas(self):
        """If the drag cannot advance, a truncated composite must fail loudly."""
        runner = self._fake_page(200, stuck=True)
        with self.assertRaises(RuntimeError):
            review._capture_full_page(runner, 'page', 368, VH)

    def test_page_seeded_mid_scroll_is_anchored_before_stitching(self):
        """A page that opens part-scrolled must be captured from its top, or
        every stripe lands on the wrong canvas rows."""
        runner = self._fake_page(226, initial_offset=150)
        path, _note = review._capture_full_page(runner, 'page', 368, VH)
        with Image.open(path) as handle:
            image = handle.convert('RGB')
        self.assertEqual(image.size, (368, VH + 226))
        pixels = image.load()
        for row in range(BOX_H + 226):
            self.assertEqual(pixels[0, BOX_Y + row], _content_color(row))

    def test_unreachable_page_top_falls_back_to_a_single_frame(self):
        """The fallback must hand back a usable path, not a nested tuple."""
        runner = self._fake_page(226, stuck=True, initial_offset=150)
        path, note = review._capture_full_page(runner, 'page', 368, VH)
        self.assertIsInstance(path, str)
        self.assertIn('page top not reachable', note)
        with Image.open(path) as handle:
            self.assertEqual(handle.size, (368, VH))

    def test_stripe_that_skips_rows_fails_instead_of_baking_a_hole(self):
        runner = self._fake_page(600, advance=500)
        with self.assertRaises(RuntimeError):
            review._capture_full_page(runner, 'page', 368, VH)

    def test_baselines_of_different_heights_never_compare_equal(self):
        with tempfile.TemporaryDirectory() as workdir:
            tall = os.path.join(workdir, 'tall.png')
            short = os.path.join(workdir, 'short.png')
            Image.new('RGB', (368, VH + 226), (1, 2, 3)).save(tall)
            Image.new('RGB', (368, VH), (1, 2, 3)).save(short)
            self.assertEqual(review._png_size(tall), (368, VH + 226))
            self.assertEqual(review._png_size(short), (368, VH))
            self.assertEqual(review._masked_diff(tall, short, []), 1.0)

    def test_scroller_inside_the_viewport_is_not_stitched(self):
        """Content clipped by its own box is not screen-clipped: leave it."""
        tree = {'type': 'lv_obj',
                'coords': {'x': 18, 'y': 74, 'w': 332, 'h': 259},
                'flags': {'scrollable': True},
                'scroll': {'dir': 12, 'top': 0, 'bottom': 245, 'y': 0}}
        self.assertIsNone(review._scroll_owner(tree, 368, VH))

    # --- label display policy ------------------------------------------------

    def _label(self, text, lines, h=96, long_mode=0, line_space=0, y=80,
               content_h=None):
        return {'type': 'lv_label',
                'coords': {'x': 16, 'y': y, 'w': 336, 'h': h},
                'flags': {'hidden': False},
                'long_mode': long_mode, 'line_space': line_space,
                'styles': {'line_height': 24, 'pad_left': 0, 'pad_right': 0},
                'content_h': h if content_h is None else content_h,
                'lines_complete': True,
                'text': text,
                'lines': [{'text': line, 'w': 200} for line in lines]}

    def test_single_line_uses_rendered_lines(self):
        """A three-line label reports the line count even when the box height
        was stretched to fit, which the height heuristic alone would accept."""
        label = self._label('网络设置', ['网络设', '置'], h=72)
        tree = {'type': 'lv_obj', 'coords': {'x': 0, 'y': 64, 'w': 368, 'h': 384},
                'flags': {'scrollable': True}, 'children': [label]}
        violations = review._lint(tree, {}, {'single_line': ['网络设置']},
                                  368, VH)
        self.assertTrue(any('wrapped single-line' in v for v in violations))

    def test_wrap_clipping_uses_content_height_after_padding(self):
        label = self._label('第一行第二行', ['第一行', '第二行'], h=55,
                            line_space=5, content_h=47)
        tree = {'type': 'lv_obj', 'coords': {'x': 0, 'y': 64, 'w': 368, 'h': 384},
                'flags': {'scrollable': True}, 'children': [label]}
        violations = review._lint(tree, {}, {}, 368, VH)
        self.assertTrue(any('clipped text' in v for v in violations))
        label['content_h'] = 53
        self.assertFalse(any('clipped text' in v
                             for v in review._lint(tree, {}, {}, 368, VH)))

    def test_missing_label_geometry_and_incomplete_lines_fail_lint(self):
        label = self._label('两行文本', ['两行', '文本'])
        del label['content_h']
        tree = {'type': 'lv_obj', 'coords': {'x': 0, 'y': 64, 'w': 368, 'h': 384},
                'flags': {'scrollable': True}, 'children': [label]}
        violations = review._lint(tree, {}, {}, 368, VH)
        self.assertTrue(any('missing label content_h' in v for v in violations))
        label['content_h'] = 96
        del label['lines_complete']
        violations = review._lint(tree, {}, {}, 368, VH)
        self.assertTrue(any('incomplete rendered lines' in v for v in violations))
        label['lines_complete'] = False
        violations = review._lint(tree, {}, {}, 368, VH)
        self.assertTrue(any('incomplete rendered lines' in v for v in violations))

    def test_clipped_wrap_is_reported(self):
        lines = ['一', '二', '三', '四']
        label = self._label('一二三四', lines, h=60)
        tree = {'type': 'lv_obj', 'coords': {'x': 0, 'y': 64, 'w': 368, 'h': 384},
                'flags': {'scrollable': True}, 'children': [label]}
        violations = review._lint(tree, {}, {}, 368, VH)
        self.assertTrue(any('clipped text' in v for v in violations))
        label['coords']['h'] = 96
        label['content_h'] = 96
        self.assertFalse(any('clipped text' in v
                             for v in review._lint(tree, {}, {}, 368, VH)))

    def test_wrap_clipped_by_fixed_parent_is_reported(self):
        label = self._label('一二三四', ['一', '二', '三', '四'], h=96, y=100)
        parent = {'type': 'lv_obj', 'coords': {'x': 0, 'y': 64, 'w': 368, 'h': 60},
                  'flags': {'scrollable': False}, 'children': [label]}
        tree = {'type': 'lv_obj', 'coords': {'x': 0, 'y': 0, 'w': 368, 'h': 448},
                'flags': {'scrollable': False}, 'children': [parent]}
        violations = review._lint(tree, {}, {}, 368, VH)
        self.assertTrue(any('clipped by parent' in v for v in violations))
        parent['flags'] = {'scrollable': True}
        parent['scroll'] = {'dir': 3}
        self.assertTrue(any('clipped by parent' in v
                            for v in review._lint(tree, {}, {}, 368, VH)))
        parent['scroll']['dir'] = 12
        self.assertFalse(any('clipped by parent' in v
                             for v in review._lint(tree, {}, {}, 368, VH)))

    def test_dots_truncation_is_reported(self):
        label = self._label('a very long neigh...', ['a very long neigh...'],
                            long_mode=1)
        tree = {'type': 'lv_obj', 'coords': {'x': 0, 'y': 64, 'w': 368, 'h': 384},
                'flags': {'scrollable': True}, 'children': [label]}
        violations = review._lint(tree, {}, {}, 368, VH)
        self.assertTrue(any('truncated text' in v for v in violations))
        self.assertFalse(any(
            'truncated text' in v
            for v in review._lint(tree, {}, {'allow_truncation': ['a very long']},
                                  368, VH)))

    def test_balanced_prose_flags_short_tail(self):
        label = self._label('大风预警。注意防范。', ['大风预警。', '注意防范。'],
                            line_space=5, h=53)
        tree = {'type': 'lv_obj', 'coords': {'x': 0, 'y': 64, 'w': 368, 'h': 384},
                'flags': {'scrollable': True}, 'children': [label]}
        state = {'balanced_prose': ['大风预警']}
        self.assertFalse(review._lint(tree, {}, state, 368, VH))
        label['lines'] = [{'text': '大风预警。', 'w': 200},
                          {'text': '。', 'w': 24}]
        self.assertTrue(any('orphan tail' in v
                            for v in review._lint(tree, {}, state, 368, VH)))
        label['lines'] = [{'text': '大风预警。', 'w': 200},
                          {'text': '，注意', 'w': 96}]
        self.assertTrue(any('line opens with' in v
                            for v in review._lint(tree, {}, state, 368, VH)))
