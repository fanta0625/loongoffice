# SPDX-License-Identifier: MPL-2.0
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import Mock, patch

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

    def test_visible_reading_uses_normal_loader_with_readonly(self):
        self._check_loader(hidden=False)

    def test_background_session_keeps_hidden_loader(self):
        self._check_loader(hidden=True)

    def _check_loader(self, *, hidden):
        context, desktop, document = Mock(), Mock(), Mock()
        context.ServiceManager.createInstanceWithContext.return_value = desktop
        desktop.loadComponentFromURL.return_value = document
        with patch('reader_session.prop', side_effect=lambda name, value: SimpleNamespace(Name=name, Value=value)), \
             patch.object(ReaderSession, '_setup_previews'):
            ReaderSession(context, 'file:///source.pdf', hidden=hidden, read_only=True)
        args = {item.Name: item.Value for item in desktop.loadComponentFromURL.call_args.args[3]}
        self.assertEqual(args['Hidden'], hidden)
        self.assertTrue(args['ReadOnly'])
        frame = document.getCurrentController().getFrame()
        if hidden:
            frame.activate.assert_not_called()
        else:
            frame.activate.assert_called_once()
        frame.getContainerWindow().setVisible.assert_not_called()

    def test_owned_layers_restored_and_edit_layer_activated(self):
        self.session._prepare_view(self.controller, self.page)
        self.assertTrue(self.session.preview_layer.IsVisible)
        self.assertTrue(self.session.preview_layer.IsLocked)
        self.assertTrue(self.session.edit_layer.IsVisible)
        self.assertFalse(self.session.edit_layer.IsLocked)
        self.controller.setPropertyValue.assert_called_once_with('ActiveLayer', self.session.edit_layer)

    def test_native_edit_transition_does_not_force_tool_or_pointer(self):
        frame = self.controller.getFrame.return_value
        self.session.document.getCurrentController.return_value = self.controller
        frame.getController().getModel.return_value = self.session.document
        self.session.page_refs = [self.page]
        self.session.current_page = Mock(return_value=1)
        native_edit = Mock()
        fake_uno = SimpleNamespace(createUnoStruct=lambda _: SimpleNamespace())
        with patch.dict(sys.modules, {'uno': fake_uno}), \
             patch('reader_session.prop', side_effect=lambda name, value: (name, value)):
            self.session.enable_editing(native_edit)
        self.assertEqual(native_edit.dispatch.call_count, 1)
        self.assertEqual(native_edit.dispatch.call_args.args[0].Complete, '.uno:EditDoc')
        frame.queryDispatch.assert_not_called()
        self.controller.select.assert_not_called()

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
