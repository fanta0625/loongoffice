# SPDX-License-Identifier: MPL-2.0
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'uno' / 'python'))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from probe_direct_edit import seed_native_undo
from reader_session import ReaderSession
from direct_edit import import_into_page, discard_resources, DirectImportUnavailable
from timing_log import TimingLog
from test_edit_copy import Page, shape


class DirectEditTests(unittest.TestCase):
    def setUp(self):
        self.session = object.__new__(ReaderSession)
        s = self.session
        s.context = Mock()
        s.document = Mock()
        s.document.hasControllersLocked.return_value = False
        s.document.isModified.return_value = True
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
