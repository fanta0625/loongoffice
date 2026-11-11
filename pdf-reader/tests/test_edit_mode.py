# SPDX-License-Identifier: MPL-2.0
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'uno' / 'python'))
from edit_mode import EditMode


class Session:
    def __init__(self):
        self.page = 1
        self.edited = set()
        self.calls = []
        self.reject = False
        self.count = 10
        self.prefetch = []

    def prefetch_page(self, page, anchor):
        self.prefetch.append((page, anchor))
        return {'event': 'prefetch-ready', 'page': page}

    def current_page(self):
        return self.page

    def edit_current_page(self):
        self.calls.append(self.page)
        if self.reject:
            raise RuntimeError('converter failed')
        self.edited.add(self.page)
        return {'page': self.page, 'reused': False}


class EditModeTests(unittest.TestCase):
    def setUp(self):
        self.session = Session()
        self.transitions = []
        self.mode = EditMode(self.session, lambda: self.transitions.append('edit'))

    def test_reading_navigation_never_imports(self):
        for page in range(1, 11):
            self.session.page = page
            self.assertIsNone(self.mode.tick(page))
        self.assertEqual(self.session.calls, [])

    def test_one_click_enables_subsequent_settled_pages(self):
        self.mode.request_edit()
        for page in (2, 7, 10):
            self.session.page = page
            self.mode.tick(page)
            self.mode.tick(page + .5)
        self.assertEqual(self.transitions, ['edit'])
        self.assertEqual(self.session.calls, [2, 7, 10])
        self.session.page = 2
        self.mode.tick(20)
        self.mode.tick(21)
        self.assertEqual(self.session.calls, [2, 7, 10])

    def test_fast_navigation_skips_intermediate_pages(self):
        self.mode.request_edit()
        for page, now in ((1, 0), (2, .1), (3, .2), (4, .3)):
            self.session.page = page
            self.mode.tick(now)
        self.mode.tick(1)
        self.assertEqual(self.session.calls, [4])

    def test_busy_thumbnail_defers_and_new_page_gets_a_full_settle_interval(self):
        self.mode.request_edit()
        self.mode.busy_reason = lambda: 'thumbnail-drag-finishing'
        self.mode.tick(0)
        self.mode.tick(2)
        self.assertEqual(self.session.calls, [])
        self.mode.busy_reason = lambda: ''
        self.session.page = 4
        self.mode.tick(3)
        self.mode.tick(3.1)
        self.assertEqual(self.session.calls, [])
        self.mode.tick(3.5)
        self.assertEqual(self.session.calls, [4])

    def test_native_deferral_is_retried_without_failure(self):
        self.mode.request_edit()
        self.mode.tick(0)
        real_edit = self.session.edit_current_page
        self.session.edit_current_page = lambda: {'deferred': True}
        self.assertIsNone(self.mode.tick(1))
        self.assertEqual(self.mode.failures, {})
        self.session.edit_current_page = real_edit
        self.assertIsNone(self.mode.tick(1.1))
        self.mode.tick(2)
        self.assertEqual(self.session.calls, [1])

    def test_failure_requires_explicit_retry(self):
        self.mode.request_edit()
        self.session.reject = True
        self.mode.tick(0)
        result = self.mode.tick(1)
        self.assertTrue(result['preview_retained'])
        self.mode.tick(100)
        self.assertEqual(self.session.calls, [1])
        self.session.reject = False
        self.mode.request_edit()
        self.mode.tick(101)
        self.assertEqual(self.session.calls, [1, 1])
        self.assertEqual(self.transitions, ['edit'])

    def test_transition_failure_does_not_enable_editing(self):
        def reject():
            raise RuntimeError('still read-only')
        self.mode.enable_editing = reject
        with self.assertRaises(RuntimeError):
            self.mode.request_edit()
        self.mode.tick(100)
        self.assertEqual(self.mode.mode, 'reading')
        self.assertEqual(self.session.calls, [])

    def test_prefetch_can_be_disabled_without_disabling_foreground(self):
        self.mode.prefetch_seconds = None
        self.mode.request_edit()
        self.mode.tick(0)
        self.mode.tick(1)
        self.mode.tick(100)
        self.assertEqual(self.session.calls, [1])
        self.assertEqual(self.session.prefetch, [])

    def test_idle_prefetch_forward_then_backward_without_editing_neighbours(self):
        self.session.page = 5
        self.mode.request_edit()
        self.mode.tick(0)
        self.mode.tick(1)
        self.assertIsNone(self.mode.tick(2))
        self.mode.tick(4)
        self.mode.tick(5)
        self.mode.tick(6)
        self.assertEqual(self.session.prefetch, [(6, 5), (4, 5)])
        self.assertEqual(self.session.edited, {5})

    def test_navigation_prioritizes_foreground_and_replaces_old_neighbours(self):
        self.mode.request_edit()
        self.mode.tick(0)
        self.mode.tick(1)
        self.session.page = 10
        self.mode.tick(4)
        self.mode.tick(5)
        self.assertEqual(self.session.calls, [1, 10])
        self.assertEqual(self.session.prefetch, [])
        self.mode.tick(8)
        self.assertEqual(self.session.prefetch, [(9, 10)])

    def test_prefetch_failure_does_not_poison_foreground_retry(self):
        self.mode.request_edit()
        self.mode.tick(0)
        self.mode.tick(1)
        def reject(*args):
            raise RuntimeError('speculative failure')
        self.session.prefetch_page = reject
        self.assertEqual(self.mode.tick(4)['event'], 'prefetch-failed')
        self.session.page = 2
        self.mode.tick(5)
        self.mode.tick(6)
        self.assertIn(2, self.session.edited)
        self.assertEqual(self.mode.failures, {})


if __name__ == '__main__':
    unittest.main()
