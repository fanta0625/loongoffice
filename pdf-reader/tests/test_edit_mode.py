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


if __name__ == '__main__':
    unittest.main()
