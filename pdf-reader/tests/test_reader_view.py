# SPDX-License-Identifier: MPL-2.0
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'uno' / 'python'))
from reader_session import ReaderSession, SessionClosed


class ReaderViewTests(unittest.TestCase):
    def setUp(self):
        self.session = object.__new__(ReaderSession)
        self.session.document = Mock()
        self.session.document.isReadonly.return_value = False
        self.session._active_view = None
        self.session.preview_layer = SimpleNamespace(IsVisible=False, IsPrintable=False, IsLocked=False)
        self.session.edit_layer = SimpleNamespace(IsVisible=False, IsPrintable=False, IsLocked=True)
        self.controller = Mock()
        self.page = object()

    def test_detached_controller_is_normal_session_close(self):
        self.session.document.getCurrentController.return_value = None
        with self.assertRaises(SessionClosed):
            self.session.current_page()

    def test_owned_layers_restored_and_edit_layer_activated(self):
        self.session._prepare_view(self.controller, self.page)
        self.assertTrue(self.session.preview_layer.IsVisible)
        self.assertTrue(self.session.preview_layer.IsLocked)
        self.assertTrue(self.session.edit_layer.IsVisible)
        self.assertFalse(self.session.edit_layer.IsLocked)
        self.controller.setPropertyValue.assert_called_once_with('ActiveLayer', self.session.edit_layer)

    def test_reading_does_not_activate_an_edit_layer(self):
        self.session.document.isReadonly.return_value = True
        self.session._prepare_view(self.controller, self.page)
        self.controller.setPropertyValue.assert_not_called()

    def test_view_flags_repaired_even_if_page_identity_does_not_change(self):
        self.session._prepare_view(self.controller, self.page)
        self.session.edit_layer.IsVisible = False
        self.session.preview_layer.IsVisible = False
        self.session._prepare_view(self.controller, self.page)
        self.assertTrue(self.session.edit_layer.IsVisible)
        self.assertTrue(self.session.preview_layer.IsVisible)
        self.controller.setPropertyValue.assert_called_once()

    def test_navigation_activates_edit_layer_on_new_page(self):
        self.session._prepare_view(self.controller, self.page)
        self.session._prepare_view(self.controller, object())
        self.assertEqual(self.controller.setPropertyValue.call_count, 2)


if __name__ == '__main__':
    unittest.main()
