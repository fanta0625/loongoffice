# SPDX-License-Identifier: MPL-2.0
"""Synchronous UNO adapter for a private PDF reading-to-editing session.

No external renderer, system clipboard, worker UNO calls or PDF save path.
This first implementation uses normal Draw page navigation, not the fixed
three-page continuous-view fixture. Tile scheduling belongs in the C++ core.
"""
from edit_bridge import open_for_edit, property_value as prop


class SessionClosed(RuntimeError):
    """The document has detached its controller during native window close."""


def set_graphic(shape, graphic):
    """Keep XGraphic's declared type across a remote Python UNO Any call."""
    import uno
    if graphic is None:
        raise RuntimeError('Internal PDF preview returned an empty XGraphic')
    uno.invoke(shape, 'setPropertyValue',
               ('Graphic', uno.Any('com.sun.star.graphic.XGraphic', graphic)))


class ReaderSession:
    def __init__(self, context, pdf_url, *, hidden=False, importer=open_for_edit, read_only=False):
        self.context, self.pdf_url, self.importer = context, pdf_url, importer
        self.edited = set()
        self.import_calls = []
        self._active_view = None
        self.desktop = context.ServiceManager.createInstanceWithContext(
            'com.sun.star.frame.Desktop', context)
        self.document = self.desktop.loadComponentFromURL(pdf_url, '_blank', 0, (
            # Visible sessions must take SfxBaseController's normal UI setup
            # path. Showing an initially hidden top-level window later skips
            # HideUI(false), child-window setup and native view activation.
            # Background conversion and headless probes remain hidden.
            prop('Hidden', hidden), prop('FilterName', 'draw_pdf_import'),
            prop('ReadOnly', read_only), prop('UpdateDocMode', 0),
            prop('MacroExecutionMode', 0),
        ))
        if self.document is None:
            raise RuntimeError('Internal PDFium graphic loading failed')
        try:
            self._setup_previews()
            if not hidden:
                self.document.getCurrentController().getFrame().activate()
        except BaseException:
            self.document.dispose()
            raise

    def _setup_previews(self):
        self.pages = self.document.getDrawPages()
        self.count = self.pages.getCount()
        if not self.count:
            raise RuntimeError('PDF has no pages')
        self.previews = []
        self.page_refs = []
        for i in range(self.count):
            page = self.pages.getByIndex(i)
            # Acquire once and retain BEFORE querying any interface. A
            # temporary getByIndex(...).getShapeType() proxy can be released
            # while the next wrapper is being acquired. SdrObject caches its
            # UNO shape weakly; do not churn wrappers around the same object.
            original = page.getByIndex(0) if page.getCount() == 1 else None
            if (original is None or original.getShapeType()
                    != 'com.sun.star.drawing.GraphicObjectShape'):
                raise RuntimeError('Expected one PDFium page graphic per page; '
                                   'annotations or a different import route are unsupported')
            # Own the preview's lifecycle instead of using an importer-owned
            # shape as a mutable session token. Keep the internal PDF Graphic
            # (including its source page), without rasterizing it or invoking
            # an editable converter. Hold the new UNO reference throughout
            # the session, and verify the page's actual object below.
            # Read everything before adding a second object to this page.
            # In particular, never pass a null Graphic into the setter:
            # SvxGraphicObject rejects it with an uninformative argument error.
            graphic = original.getPropertyValue('Graphic')
            position, size = original.getPosition(), original.getSize()
            if graphic is None:
                raise RuntimeError(f'Internal PDF preview page {i + 1} has no XGraphic '
                                   f'before initialization (objects={page.getCount()}, '
                                   f'name={original.getName()!r}, layer={original.LayerName!r})')
            preview = self.document.createInstance('com.sun.star.drawing.GraphicObjectShape')
            page.add(preview)
            set_graphic(preview, graphic)
            preview.setPosition(position)
            preview.setSize(size)
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
        self.preview_layer = preview_layer
        self.edit_layer = layers.insertNewByIndex(layers.getCount())
        self.edit_layer.Name = 'PDF imported edit objects'
        self.edit_layer.IsVisible = self.edit_layer.IsPrintable = True
        self.edit_layer.IsLocked = False
        for number in range(1, self.count + 1):
            self._reading_preview(number, phase='initialization')
        self.document.getUndoManager().clear()  # Initial session setup only.
        self.document.setModified(False)
        self.document.getCurrentController().setCurrentPage(self.pages.getByIndex(0))

    def current_page(self):
        controller = self.document.getCurrentController()
        if controller is None:
            raise SessionClosed('PDF window no longer has a controller')
        current = controller.getCurrentPage()
        if current is None:
            raise RuntimeError('Draw controller has no active page')
        if self.pages.getCount() != self.count:
            raise RuntimeError('Page insertion/deletion is unsupported in this session')
        prefix = 'PDF source page '
        name = current.Name
        if name.startswith(prefix) and name[len(prefix):].isdigit():
            number = int(name[len(prefix):])
            if (1 <= number <= self.count and name == f'{prefix}{number}'
                    and self.pages.getByIndex(number - 1) == current
                    and self.page_refs[number - 1] == current):
                self._prepare_view(controller, current)
                return number
        raise RuntimeError('Page rename/reorder is unsupported; select an unchanged source page')

    def _prepare_view(self, controller, page):
        # SdLayer reads/writes the CURRENT SdrPageView as well as FrameView.
        # Setting flags once while hidden does not validate later page views,
        # nor the rebuilt view after ReadOnly -> EditDoc. Restore only our two
        # owned layers, never unlock unrelated document layers.
        for layer, locked in ((self.preview_layer, True), (self.edit_layer, False)):
            if not layer.IsVisible:
                layer.IsVisible = True
            if not layer.IsPrintable:
                layer.IsPrintable = True
            if layer.IsLocked != locked:
                layer.IsLocked = locked
        key = (controller, page)
        if self._active_view != key:
            if not self.document.isReadonly():
                controller.setPropertyValue('ActiveLayer', self.edit_layer)
            self._active_view = key

    def view_state(self):
        controller = self.document.getCurrentController()
        if controller is None:
            raise SessionClosed('PDF window no longer has a controller')
        active = controller.getPropertyValue('ActiveLayer')
        return {'read_only': self.document.isReadonly(),
                'active_layer': active.Name if active else None,
                'preview_visible': self.preview_layer.IsVisible,
                'preview_locked': self.preview_layer.IsLocked,
                'edit_visible': self.edit_layer.IsVisible,
                'edit_locked': self.edit_layer.IsLocked}

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
        if self.document.isReadonly():
            raise RuntimeError('Enter document edit mode before converting pages')
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
            return {'page': page_number, 'reused': False, 'shapes': len(shapes),
                    'view': self.view_state()}
        finally:
            staged.dispose()

    def close(self):
        self.document.dispose()

    def enable_editing(self, native_dispatch):
        """Use the native edit command, bypassing our frame interceptor once.

        Refuse a product that reloads/replaces the model: all page identities,
        preview ownership and later edit history must remain valid.
        """
        import time
        import uno
        page = self.current_page()
        frame = self.document.getCurrentController().getFrame()
        url = uno.createUnoStruct('com.sun.star.util.URL')
        url.Complete = '.uno:EditDoc'
        url.Main, url.Protocol, url.Path = '.uno:EditDoc', '.uno:', 'EditDoc'
        native_dispatch.dispatch(url, (prop('Edit', True),))
        deadline = time.monotonic() + 10
        while True:
            if frame.getController().getModel() != self.document:
                raise RuntimeError('Native edit command replaced the reading model; unsupported Office build')
            if not self.document.isReadonly():
                break
            if time.monotonic() >= deadline:
                raise RuntimeError('Native edit command did not leave read-only mode')
            time.sleep(.05)
        self.document.getCurrentController().setCurrentPage(self.page_refs[page - 1])
        self._active_view = None
        self.current_page()  # Validate retained page mapping before any import.
        # Leave any reading/navigation tool and restore the native object
        # selection tool once, without stealing focus on every page poll.
        select = uno.createUnoStruct('com.sun.star.util.URL')
        select.Complete = select.Main = '.uno:SelectObject'
        select.Protocol, select.Path = '.uno:', 'SelectObject'
        dispatch = frame.queryDispatch(select, '', 0)
        if dispatch is not None:
            dispatch.dispatch(select, ())
