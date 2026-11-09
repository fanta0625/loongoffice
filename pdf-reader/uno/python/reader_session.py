# SPDX-License-Identifier: MPL-2.0
"""Synchronous UNO adapter for a private PDF reading-to-editing session.

No external renderer, system clipboard, worker UNO calls or PDF save path.
This first implementation uses normal Draw page navigation, not the fixed
three-page continuous-view fixture. Tile scheduling belongs in the C++ core.
"""
from edit_bridge import open_for_edit, property_value as prop


class ReaderSession:
    def __init__(self, context, pdf_url, *, hidden=False, importer=open_for_edit):
        self.context, self.pdf_url, self.importer = context, pdf_url, importer
        self.edited = set()
        self.import_calls = []
        self.desktop = context.ServiceManager.createInstanceWithContext(
            'com.sun.star.frame.Desktop', context)
        self.document = self.desktop.loadComponentFromURL(pdf_url, '_blank', 0, (
            prop('Hidden', True), prop('FilterName', 'draw_pdf_import'),
            prop('ReadOnly', False), prop('UpdateDocMode', 0),
            prop('MacroExecutionMode', 0),
        ))
        if self.document is None:
            raise RuntimeError('Internal PDFium graphic loading failed')
        try:
            self.pages = self.document.getDrawPages()
            self.count = self.pages.getCount()
            if not self.count:
                raise RuntimeError('PDF has no pages')
            self.previews = []
            self.page_refs = []
            for i in range(self.count):
                page = self.pages.getByIndex(i)
                if (page.getCount() != 1 or page.getByIndex(0).getShapeType()
                        != 'com.sun.star.drawing.GraphicObjectShape'):
                    raise RuntimeError('Expected one PDFium page graphic per page; '
                                       'annotations or a different import route are unsupported')
                # Own the preview's lifecycle instead of using an importer-owned
                # shape as a mutable session token. Keep the internal PDF Graphic
                # (including its source page), without rasterizing it or invoking
                # an editable converter. Hold the new UNO reference throughout
                # the session, and verify the page's actual object below.
                original = page.getByIndex(0)
                preview = self.document.createInstance('com.sun.star.drawing.GraphicObjectShape')
                page.add(preview)
                preview.setPropertyValue('Graphic', original.getPropertyValue('Graphic'))
                preview.setPosition(original.getPosition())
                preview.setSize(original.getSize())
                preview.setName(f'PDF reader preview {i + 1}')
                preview.setPropertyValue('MoveProtect', True)
                preview.setPropertyValue('SizeProtect', True)
                page.remove(original)
                self.previews.append(preview)
                self.page_refs.append(page)
                page.Name = f'PDF source page {i + 1}'
            layers = self.document.getLayerManager()
            preview_layer = layers.insertNewByIndex(layers.getCount())
            preview_layer.Name = 'PDF reader locked previews'
            preview_layer.IsVisible = preview_layer.IsPrintable = True
            for shape in self.previews:
                layers.attachShapeToLayer(shape, preview_layer)
            preview_layer.IsLocked = True
            self.edit_layer = layers.insertNewByIndex(layers.getCount())
            self.edit_layer.Name = 'PDF imported edit objects'
            self.edit_layer.IsVisible = self.edit_layer.IsPrintable = True
            self.edit_layer.IsLocked = False
            for number in range(1, self.count + 1):
                self._reading_preview(number, phase='initialization')
            self.document.getUndoManager().clear()  # Initial session setup only.
            self.document.setModified(False)
            self.document.getCurrentController().setCurrentPage(self.pages.getByIndex(0))
            if not hidden:
                self.document.getCurrentController().getFrame().getContainerWindow().setVisible(True)
        except BaseException:
            self.document.dispose()
            raise

    def current_page(self):
        current = self.document.getCurrentController().getCurrentPage()
        if self.pages.getCount() != self.count:
            raise RuntimeError('Page insertion/deletion is unsupported in this session')
        prefix = 'PDF source page '
        name = current.Name
        if name.startswith(prefix) and name[len(prefix):].isdigit():
            number = int(name[len(prefix):])
            if (1 <= number <= self.count and name == f'{prefix}{number}'
                    and self.pages.getByIndex(number - 1) == current
                    and self.page_refs[number - 1] == current):
                return number
        raise RuntimeError('Page rename/reorder is unsupported; select an unchanged source page')

    def _attach_edit_layer(self, shape):
        self.document.getLayerManager().attachShapeToLayer(shape, self.edit_layer)
        if shape.supportsService('com.sun.star.drawing.GroupShape'):
            for i in range(shape.getCount()):
                self._attach_edit_layer(shape.getByIndex(i))

    def _reading_preview(self, page_number, *, phase='before conversion'):
        page = self.pages.getByIndex(page_number - 1)
        preview = page.getByIndex(0) if page.getCount() == 1 else None
        if (preview is None or preview.getShapeType() != 'com.sun.star.drawing.GraphicObjectShape'
                or preview.getName() != f'PDF reader preview {page_number}'
                or preview.getPropertyValue('LayerName') != 'PDF reader locked previews'
                or preview != self.previews[page_number - 1]):
            details = ('multiple/no objects' if preview is None else
                       f'{preview.getShapeType()}, name={preview.getName()!r}, '
                       f'layer={preview.getPropertyValue("LayerName")!r}, '
                       f'same_object={preview == self.previews[page_number - 1]}')
            raise RuntimeError(f'Reading preview integrity check failed on page {page_number} '
                               f'({phase}); refusing to replace objects: ' + details)
        return preview

    def edit_current_page(self):
        page_number = self.current_page()
        if page_number in self.edited:
            return {'page': page_number, 'reused': True}
        page = self.pages.getByIndex(page_number - 1)
        preview = self._reading_preview(page_number)
        # Conversion is staged before touching the reading document. Only the
        # requested source page may reach the edit converter's displayPage().
        self.import_calls.append(page_number)
        staged = self.importer(self.context, self.pdf_url, hidden=True, page=page_number)
        added = []
        try:
            self._reading_preview(page_number, phase='after staged import')
            imported = staged.getDrawPages()
            if imported.getCount() != 1:
                raise RuntimeError('Selected-page import returned more than one page')
            source = imported.getByIndex(0)
            if abs(source.Width - page.Width) > 30 or abs(source.Height - page.Height) > 30:
                raise RuntimeError('Imported geometry differs from reading page; crop/rotation needs adaptation')
            shapes = [source.getByIndex(i) for i in range(source.getCount())]
            # SvxDrawPage::add clones foreign-model SdrObjects into the target
            # model, preserving geometry/styles; no global clipboard is used.
            # Conversion is a baseline transition, not an undo step. Never clear
            # history belonging to edits on other pages.
            undo = self.document.getUndoManager()
            was_modified = self.document.isModified()
            undo.lock()
            try:
                try:
                    for shape in shapes:
                        old_count = page.getCount()
                        try:
                            page.add(shape)
                        finally:
                            added.extend(page.getByIndex(i) for i in range(old_count, page.getCount()))
                        self._attach_edit_layer(added[-1])
                    if page.getCount() != len(shapes) + 1:
                        raise RuntimeError('Object cloning did not preserve shape count')
                    page.remove(preview)  # Commit only after every clone succeeded.
                except BaseException:
                    for shape in reversed(added):
                        page.remove(shape)
                    self.document.setModified(was_modified)
                    raise
            finally:
                undo.unlock()
            self.edited.add(page_number)
            self.document.setModified(True)
            self.document.getCurrentController().setCurrentPage(page)
            return {'page': page_number, 'reused': False, 'shapes': len(shapes)}
        finally:
            staged.dispose()

    def close(self):
        self.document.dispose()
