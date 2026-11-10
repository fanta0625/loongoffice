# SPDX-License-Identifier: MPL-2.0
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'uno' / 'python'))
from edit_copy import CopyProfile, controller_batch
from reader_session import ReaderSession, ConversionCancelled
from timing_log import TimingLog


class Page:
    def __init__(self, preview):
        self.shapes = [preview]
        self.after_add = lambda: None

    def getCount(self):
        return len(self.shapes)

    def getByIndex(self, index):
        return self.shapes[index]

    def add(self, shape):
        self.shapes.append(shape)
        self.after_add()

    def remove(self, shape):
        self.shapes.remove(shape)


def shape():
    value = Mock()
    value.getPosition.return_value = SimpleNamespace(X=17, Y=23)
    value.getSize.return_value = SimpleNamespace(Width=43, Height=59)
    value.supportsService.return_value = False
    return value


class EditCopyTests(unittest.TestCase):
    def setUp(self):
        self.session = object.__new__(ReaderSession)
        self.document = self.session.document = Mock()
        self.document.hasControllersLocked.return_value = False
        self.document.isModified.return_value = True
        self.session.layer_manager = Mock()
        self.session.edit_layer = object()
        self.session.current_page = Mock(return_value=1)
        self.preview = object()
        self.page = Page(self.preview)
        self.controller = self.document.getCurrentController.return_value
        self.controller.getCurrentPage.return_value = self.page
        self.profile = CopyProfile(TimingLog(), 1)

    def copy(self, shapes):
        self.session._copy_edit_shapes(shapes, self.page, self.preview, 1, self.profile)

    def assert_recovered(self):
        self.assertEqual(self.page.shapes, [self.preview])
        self.assertEqual(self.document.lockControllers.call_count,
                         self.document.unlockControllers.call_count)
        self.document.getUndoManager().unlock.assert_called_once()
        self.document.getUndoManager().clear.assert_not_called()
        self.document.setModified.assert_called_with(True)

    def test_batches_preserve_geometry_and_existing_undo_history(self):
        shapes = [shape() for _ in range(33)]
        with patch('reader_session.COPY_BATCH_SECONDS', 100):
            self.copy(shapes)
        self.assertEqual(self.page.shapes, shapes)
        self.assertEqual(self.profile.batches, 3)
        self.assertEqual(self.document.unlockControllers.call_count, 3)
        self.assertEqual(self.session.current_page.call_count, 4)
        self.assertEqual(self.profile.calls['native_add'], 33)
        self.document.getLayerManager.assert_not_called()
        self.document.getUndoManager().clear.assert_not_called()
        for value in shapes:
            value.setPosition.assert_called_with(value.getPosition.return_value)
            value.setSize.assert_called_with(value.getSize.return_value)

    def test_time_budget_unlocks_after_each_slow_object(self):
        with patch('reader_session.COPY_BATCH_SECONDS', 0):
            self.copy([shape() for _ in range(3)])
        self.assertEqual(self.profile.batches, 3)
        self.assertEqual(self.document.unlockControllers.call_count, 3)

    def test_navigation_within_batch_cancels_and_restores_preview(self):
        def navigate():
            self.controller.getCurrentPage.return_value = object()
        self.page.after_add = navigate
        with self.assertRaises(ConversionCancelled):
            self.copy([shape(), shape()])
        self.assert_recovered()

    def test_throw_after_insertion_still_rolls_back_clone(self):
        def fail():
            raise RuntimeError('injected add failure')
        self.page.after_add = fail
        with self.assertRaisesRegex(RuntimeError, 'add failure'):
            self.copy([shape()])
        self.assert_recovered()

    def test_geometry_mismatch_rolls_back(self):
        value = shape()
        def distort():
            value.getPosition.return_value = SimpleNamespace(X=999, Y=23)
        self.page.after_add = distort
        with self.assertRaisesRegex(RuntimeError, 'geometry changed'):
            self.copy([value])
        self.assert_recovered()

    def test_layer_failure_rolls_back(self):
        self.session.layer_manager.attachShapeToLayer.side_effect = RuntimeError('layer failure')
        with self.assertRaisesRegex(RuntimeError, 'layer failure'):
            self.copy([shape()])
        self.assert_recovered()

    def test_outer_controller_lock_is_not_released(self):
        self.document.hasControllersLocked.return_value = True
        with self.assertRaisesRegex(RuntimeError, 'test failure'):
            with controller_batch(self.document, self.profile):
                raise RuntimeError('test failure')
        self.document.lockControllers.assert_not_called()
        self.document.unlockControllers.assert_not_called()

    def test_group_children_keep_edit_layer(self):
        group, child = shape(), shape()
        group.supportsService.return_value = True
        group.getCount.return_value = 1
        group.getByIndex.return_value = child
        self.copy([group])
        self.assertEqual(self.session.layer_manager.attachShapeToLayer.call_count, 2)
        self.session.layer_manager.attachShapeToLayer.assert_called_with(child, self.session.edit_layer)


if __name__ == '__main__':
    unittest.main()
