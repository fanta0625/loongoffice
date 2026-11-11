# SPDX-License-Identifier: MPL-2.0
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'uno' / 'python'))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from probe_direct_edit import seed_native_undo, prepare_ui_text_probe, verify_ui_typing_undo
from reader_session import ReaderSession
from direct_edit import import_into_page, discard_resources, DirectImportUnavailable, DIRECT_IMPORTER
from timing_log import TimingLog
from test_edit_copy import Page, shape


class DirectEditTests(unittest.TestCase):
    def test_ui_typing_accepts_extra_spaces_and_checks_exact_undo_redo(self):
        original = 'PDF SOURCE PAGE 2'
        typed = original + '  ZUNDO'
        text = Mock()
        text.getString.side_effect = [original, typed, original, typed]
        with patch('builtins.input', return_value=''), patch('builtins.print'):
            verify_ui_typing_undo(text)

    def test_ui_typing_rejects_changes_to_original_text(self):
        text = Mock()
        text.getString.return_value = 'PDF SOURCE PAGE 2'
        with patch('builtins.input', return_value=''), patch('builtins.print'):
            with self.assertRaisesRegex(RuntimeError, 'Expected original text'):
                verify_ui_typing_undo(text)

    def setUp(self):
        self.session = object.__new__(ReaderSession)
        s = self.session
        s.guard_automatic_edit = False
        s.context = Mock()
        s.document = Mock()
        s.document.hasControllersLocked.return_value = False
        s.document.isModified.return_value = True
        s.document.isSetModifiedEnabled.return_value = True
        s.document.disableSetModified.side_effect = lambda: setattr(
            s.document.isSetModifiedEnabled, 'return_value', False)
        s.document.enableSetModified.side_effect = lambda: setattr(
            s.document.isSetModifiedEnabled, 'return_value', True)
        s.pdf_url = 'file:///fixture.pdf'
        s.timing = TimingLog()
        s.count, s.edited, s.import_calls = 3, set(), []
        s.edit_layer = SimpleNamespace(Name='PDF imported edit objects')
        self.preview = object()
        self.page = Page(self.preview)
        self.page.Name = 'PDF source page 2'
        s.previews = [None, self.preview, None]
        s.pages = Mock()
        s.pages.getCount.return_value = 3
        s.pages.getByIndex.return_value = self.page
        s._reading_preview = Mock(return_value=self.preview)
        s.view_state = Mock(return_value={})
        self.controller = s.document.getCurrentController.return_value
        self.controller.getCurrentPage.return_value = self.page

    def run_import(self, action):
        with patch('reader_session.import_into_page', side_effect=action) as importer, \
                patch('reader_session.discard_resources') as cleanup:
            result = self.session._edit_direct_page(2)
        return result, importer, cleanup

    def assert_undo_retained(self):
        self.session.document.getUndoManager().clear.assert_not_called()
        self.session.document.getUndoManager().unlock.assert_called_once()
        self.session.document.unlockControllers.assert_called_once()
        self.assertTrue(self.session.document.isSetModifiedEnabled())

    def test_native_updates_suppressed_until_commit_then_published_once(self):
        document = self.session.document
        def import_page(*args):
            self.assertFalse(document.isSetModifiedEnabled())
            self.page.shapes.append(shape())
        def publish(value):
            self.assertTrue(document.isSetModifiedEnabled())
            self.assertEqual(len(self.page.shapes), 1)
            self.assertNotIn(self.preview, self.page.shapes)
            self.assertTrue(value)
        document.setModified.side_effect = publish
        self.run_import(import_page)
        document.disableSetModified.assert_called_once()
        document.enableSetModified.assert_called_once()
        document.setModified.assert_called_once_with(True)

    def test_failure_restores_clean_state_after_suppressed_rollback(self):
        document = self.session.document
        document.isModified.return_value = False
        original_remove = self.page.remove
        def remove(obj):
            self.assertFalse(document.isSetModifiedEnabled())
            original_remove(obj)
        self.page.remove = remove
        def fail(*args):
            self.page.shapes.append(shape())
            raise RuntimeError('injected failure')
        with self.assertRaisesRegex(RuntimeError, 'injected failure'):
            self.run_import(fail)
        self.assertEqual(self.page.shapes, [self.preview])
        document.setModified.assert_called_once_with(False)
        self.assert_undo_retained()

    def test_outer_modification_suppression_not_released_or_published(self):
        document = self.session.document
        document.isSetModifiedEnabled.return_value = False
        self.run_import(lambda *args: self.page.shapes.append(shape()))
        document.disableSetModified.assert_not_called()
        document.enableSetModified.assert_not_called()
        document.setModified.assert_not_called()
        self.assertFalse(document.isSetModifiedEnabled())

    def test_ui_probe_restores_native_selector_and_selects_imported_text(self):
        controller = self.session.document.getCurrentController.return_value
        native = controller.getFrame.return_value.queryDispatch.return_value
        self.session.view_state.return_value = {'read_only': False, 'edit_locked': False}
        fake_ui = SimpleNamespace(command_url=lambda command: command)
        with patch.dict(sys.modules, {'reader_infobar': fake_ui}):
            prepare_ui_text_probe(self.session.context, self.session, 'imported-text')
        controller.getFrame().activate.assert_called_once()
        controller.getFrame().getComponentWindow().setFocus.assert_called_once()
        native.dispatch.assert_called_once_with('.uno:SelectObject', ())
        controller.select.assert_called_once_with('imported-text')
        self.session.document.getUndoManager().clear.assert_not_called()

    def test_success_creates_once_without_copy_or_shape_enumeration(self):
        objects = [shape() for _ in range(100)]
        self.page.getByIndex = Mock(side_effect=AssertionError('success must not enumerate shapes'))
        result, importer, cleanup = self.run_import(lambda *args: self.page.shapes.extend(objects))
        self.assertEqual(self.page.shapes, objects)
        self.assertEqual(result['shapes'], 100)
        self.assertEqual(result['copied_shapes'], 0)
        self.assertEqual(result['temporary_draw_documents'], 0)
        self.assertEqual(self.session.edited, {2})
        importer.assert_called_once()
        self.assertNotIn('automatic', importer.call_args.kwargs)
        cleanup.assert_not_called()
        for obj in objects:
            obj.setPosition.assert_not_called()
            obj.setSize.assert_not_called()
        self.assert_undo_retained()

    def test_partial_failure_removes_new_shapes_and_resources(self):
        def fail(*args):
            self.page.shapes.extend([shape(), shape()])
            raise RuntimeError('native failure')
        with patch('reader_session.import_into_page', side_effect=fail), \
                patch('reader_session.discard_resources') as cleanup:
            with self.assertRaisesRegex(RuntimeError, 'native failure'):
                self.session._edit_direct_page(2)
        self.assertEqual(self.page.shapes, [self.preview])
        self.assertFalse(self.session.edited)
        cleanup.assert_called_once()
        self.session.document.setModified.assert_called_with(True)
        self.assert_undo_retained()

    def test_native_deferral_never_rolls_back_or_marks_preview_modified(self):
        from direct_edit import DirectImportDeferred
        def defer(*args, **kwargs):
            raise DirectImportDeferred()
        self.session.guard_automatic_edit = True
        result, importer, cleanup = self.run_import(defer)
        self.assertTrue(result['deferred'])
        self.assertTrue(importer.call_args.kwargs['automatic'])
        self.assertEqual(self.page.shapes, [self.preview])
        self.assertEqual(self.session.import_calls, [])
        self.assertFalse(self.session.edited)
        self.session.document.setModified.assert_not_called()
        cleanup.assert_not_called()
        self.assert_undo_retained()

    def test_navigation_after_native_call_rolls_back_before_commit(self):
        def navigate(*args):
            self.page.shapes.append(shape())
            self.controller.getCurrentPage.return_value = object()
        result, _, cleanup = self.run_import(navigate)
        self.assertTrue(result['cancelled'])
        self.assertEqual(self.page.shapes, [self.preview])
        self.assertFalse(self.session.edited)
        cleanup.assert_called_once()
        self.assert_undo_retained()

    def test_missing_service_never_opens_or_mutates_document(self):
        self.session.context.ServiceManager.getAvailableServiceNames.return_value = ()
        with self.assertRaises(DirectImportUnavailable):
            import_into_page(self.session.context, self.session.document, self.page,
                             'file:///fixture.pdf', 2, 'edit', 'private-')
        self.session.context.ServiceManager.createInstanceWithContext.assert_not_called()
        self.session.document.assert_not_called()

    def native_filter(self, result=True):
        context = Mock()
        context.ServiceManager.getAvailableServiceNames.return_value = (DIRECT_IMPORTER,)
        importer = context.ServiceManager.createInstanceWithArgumentsAndContext.return_value
        importer.filter.return_value = result
        return context, importer

    def test_bridge_uses_page_filter_without_raw_converter_or_python_stream(self):
        context, importer = self.native_filter()
        with patch('direct_edit.prop', side_effect=lambda name, value: (name, value)):
            import_into_page(context, self.session.document, self.page,
                             'file:///fixture.pdf', 2, 'edit', 'private-')
        importer.setTargetDocument.assert_called_once_with(self.session.document)
        importer.filter.assert_called_once_with((('URL', 'file:///fixture.pdf'), ('SourcePage', 2)))
        context.ServiceManager.createInstanceWithContext.assert_not_called()

    def test_bridge_false_result_is_failure_not_silent_copy_fallback(self):
        context, _ = self.native_filter(False)
        with patch('direct_edit.prop', side_effect=lambda name, value: (name, value)):
            with self.assertRaisesRegex(RuntimeError, 'returned false'):
                import_into_page(context, self.session.document, self.page,
                                 'file:///fixture.pdf', 2, 'edit', 'private-')
        context.ServiceManager.createInstanceWithContext.assert_not_called()

    def test_rollback_removes_only_request_styles_and_resources(self):
        document = Mock()
        dash, bitmap, graphics = Mock(), Mock(), Mock()
        document.createInstance.side_effect = [dash, bitmap]
        document.getStyleFamilies.return_value.getByName.return_value = graphics
        for table in (dash, bitmap, graphics):
            table.getElementNames.return_value = ('standard', 'other-standard', 'request-standard')
        discard_resources(None, document, 'request-')
        for table in (dash, bitmap, graphics):
            table.removeByName.assert_called_once_with('request-standard')

    def test_native_undo_probe_detects_leaked_lock_before_mutation(self):
        document = Mock()
        document.getUndoManager.return_value.isLocked.return_value = True
        with self.assertRaisesRegex(RuntimeError, 'locked'):
            seed_native_undo(Mock(), document, Mock())
        document.createInstance.assert_not_called()

    def test_native_undo_probe_uses_draw_dispatch(self):
        document, context = Mock(), Mock()
        document.getUndoManager.return_value.isLocked.return_value = False
        document.getUndoManager.return_value.isUndoPossible.return_value = True
        document.getCurrentController.return_value.select.return_value = True
        page = Page(self.preview)
        marker = document.createInstance.return_value
        dispatch = context.ServiceManager.createInstanceWithContext.return_value
        dispatch.executeDispatch.side_effect = lambda *args: page.remove(marker)
        self.assertIs(seed_native_undo(context, document, page), marker)
        self.assertEqual(dispatch.executeDispatch.call_args.args[1], '.uno:Delete')
        document.getUndoManager.return_value.addUndoAction.assert_not_called()


if __name__ == '__main__':
    unittest.main()
