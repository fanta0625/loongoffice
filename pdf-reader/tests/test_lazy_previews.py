# SPDX-License-Identifier: MPL-2.0
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'uno' / 'python'))
from reader_session import ReaderSession
from timing_log import TimingLog


class Page:
    def __init__(self, shape):
        self.shapes = [shape]
        self.Name = ''

    def getCount(self):
        return len(self.shapes)

    def getByIndex(self, index):
        return self.shapes[index]

    def add(self, shape):
        self.shapes.append(shape)

    def remove(self, shape):
        self.shapes.remove(shape)


class LazyPreviewTests(unittest.TestCase):
    def setUp(self):
        self.session = object.__new__(ReaderSession)
        self.session.timing = TimingLog()
        self.session.edited = set()
        self.session._active_view = None
        self.document = self.session.document = Mock()
        self.document.isModified.return_value = True
        self.originals = []
        for i in range(500):
            shape = Mock()
            shape.getShapeType.return_value = 'com.sun.star.drawing.GraphicObjectShape'
            shape.getPropertyValue.return_value = f'graphic-{i + 1}'
            self.originals.append(shape)
        self.pages = [Page(shape) for shape in self.originals]
        pages = self.document.getDrawPages.return_value
        pages.getCount.return_value = 500
        pages.getByIndex.side_effect = self.pages.__getitem__
        self.document.getLayerManager().insertNewByIndex.side_effect = (
            SimpleNamespace(), SimpleNamespace())
        self.document.createInstance.side_effect = lambda _: Mock()
        self.setter = patch('reader_session.set_graphic')
        self.set_graphic = self.setter.start()
        self.addCleanup(self.setter.stop)
        self.session._setup_previews()
        self.document.getUndoManager().reset_mock()

    def test_startup_only_materializes_first_page(self):
        self.assertEqual(sum(p is not None for p in self.session.previews), 1)
        self.assertEqual(self.document.createInstance.call_count, 1)
        for shape in self.originals[1:]:
            shape.getPropertyValue.assert_not_called()
        self.assertIs(self.session.page_refs[449], self.pages[449])

    def test_seek_and_revisit_skip_intermediate_pages_and_keep_history(self):
        self.document.getCurrentController().getCurrentPage.return_value = self.pages[449]
        self.assertEqual(self.session.current_page(), 450)
        self.assertEqual(self.session.current_page(), 450)
        self.assertEqual(self.document.createInstance.call_count, 2)
        for shape in self.originals[1:449]:
            shape.getPropertyValue.assert_not_called()
        self.document.getUndoManager().clear.assert_not_called()
        self.document.setModified.assert_called_with(True)
        self.document.getUndoManager().lock.assert_called_once()
        self.document.getUndoManager().unlock.assert_called_once()

    def test_failure_keeps_original_and_can_retry(self):
        self.set_graphic.side_effect = RuntimeError('injected assignment failure')
        with self.assertRaisesRegex(RuntimeError, 'assignment failure'):
            self.session.ensure_preview(450)
        self.assertEqual(self.pages[449].shapes, [self.originals[449]])
        self.assertIsNone(self.session.previews[449])
        self.document.getUndoManager().unlock.assert_called_once()
        self.document.getUndoManager().clear.assert_not_called()
        self.set_graphic.side_effect = None
        self.session.ensure_preview(450)
        self.assertIsNotNone(self.session.previews[449])

    def test_replaced_or_reordered_unvisited_page_is_rejected(self):
        self.pages[449].shapes = [Mock()]
        with self.assertRaisesRegex(RuntimeError, 'refusing to replace'):
            self.session.ensure_preview(450)
        self.pages[449].shapes = [self.originals[449]]
        self.pages[449], self.pages[448] = self.pages[448], self.pages[449]
        with self.assertRaisesRegex(RuntimeError, 'refusing to replace'):
            self.session.ensure_preview(450)

    def test_edited_page_is_never_reinitialized(self):
        self.session.edited.add(450)
        self.session.ensure_preview(450)
        self.assertEqual(self.document.createInstance.call_count, 1)


if __name__ == '__main__':
    unittest.main()
