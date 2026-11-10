# SPDX-License-Identifier: MPL-2.0
"""Synchronous UNO adapter for a private PDF reading-to-editing session.

No external renderer, system clipboard, worker UNO calls or PDF save path.
This first implementation uses normal Draw page navigation, not the fixed
three-page continuous-view fixture. No tile rendering is implemented here.
"""
from edit_bridge import open_for_edit, property_value as prop
from edit_cache import StagedPageCache
from edit_copy import (CopyProfile, controller_batch, COPY_BATCH_SIZE,
                       COPY_BATCH_MIN_SIZE, COPY_BATCH_SECONDS)
from timing_log import TimingLog
from direct_edit import import_into_page, discard_resources
import time
import uuid


class SessionClosed(RuntimeError):
    """The document has detached its controller during native window close."""


class ConversionCancelled(RuntimeError):
    """Navigation invalidated an uncommitted shape-copy transaction."""


def set_graphic(shape, graphic):
    """Keep XGraphic's declared type across a remote Python UNO Any call."""
    import uno
    if graphic is None:
        raise RuntimeError('Internal PDF preview returned an empty XGraphic')
    uno.invoke(shape, 'setPropertyValue',
               ('Graphic', uno.Any('com.sun.star.graphic.XGraphic', graphic)))


class ReaderSession:
    def __init__(self, context, pdf_url, *, hidden=False, importer=open_for_edit, read_only=False,
                 timing=None, direct_edit=False):
        self.timing = timing or TimingLog()
        self.context, self.pdf_url, self.importer = context, pdf_url, importer
        self.direct_edit = direct_edit
        self.edited = set()
        self.import_calls = []
        self.staged_cache = StagedPageCache()
        self._active_view = None
        self.desktop = context.ServiceManager.createInstanceWithContext(
            'com.sun.star.frame.Desktop', context)
        with self.timing.span('pdf.load_component'):
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
            with self.timing.span('previews.setup'):
                self._setup_previews()
            with self.timing.span('window.activate'):
                if not hidden:
                    self.document.getCurrentController().getFrame().activate()
        except BaseException:
            self.document.dispose()
            raise

    def _setup_previews(self):
        self.pages = self.document.getDrawPages()
        self.count = self.pages.getCount()
        self.timing.mark('previews.enumerated', pages=self.count)
        if not self.count:
            raise RuntimeError('PDF has no pages')
        self.previews = [None] * self.count
        self.page_refs = []
        self._originals = []
        # Keep stable page/shape wrappers and source-page identities, but do
        # not fetch XGraphic or clone a preview for every page at startup.
        with self.timing.span('previews.register', pages=self.count):
            for i in range(self.count):
                page = self.pages.getByIndex(i)
                original = page.getByIndex(0) if page.getCount() == 1 else None
                if (original is None or original.getShapeType()
                        != 'com.sun.star.drawing.GraphicObjectShape'):
                    raise RuntimeError('Expected one PDFium page graphic per page; '
                                       'annotations or a different import route are unsupported')
                self.page_refs.append(page)
                self._originals.append(original)
                page.Name = f'PDF source page {i + 1}'
        phase_started = time.monotonic()
        self.timing.mark('previews.layers.begin')
        layers = self.document.getLayerManager()
        self.layer_manager = layers
        preview_layer = layers.insertNewByIndex(layers.getCount())
        preview_layer.Name = 'PDF reader locked previews'
        preview_layer.IsVisible = preview_layer.IsPrintable = True
        preview_layer.IsLocked = True
        self.preview_layer = preview_layer
        self.edit_layer = layers.insertNewByIndex(layers.getCount())
        self.edit_layer.Name = 'PDF imported edit objects'
        self.edit_layer.IsVisible = self.edit_layer.IsPrintable = True
        self.edit_layer.IsLocked = False
        self.timing.mark('previews.layers.end', duration_ms=round((time.monotonic() - phase_started) * 1000, 2))
        # Only the first page is prepared before the viewport loop starts.
        self.ensure_preview(1)
        self.timing.mark('previews.finalize.begin')
        self.document.getUndoManager().clear()  # Initial session setup only.
        self.document.setModified(False)
        self.document.getCurrentController().setCurrentPage(self.pages.getByIndex(0))
        self.timing.mark('previews.finalize.end')

    def ensure_preview(self, number):
        """Prepare exactly this source page, without walking intermediate pages.

        Retain the original until commit; failures leave it on the page. Lazy
        setup must not create undo entries or clear edits on other pages.
        """
        if not 1 <= number <= self.count:
            raise ValueError('PDF page out of range')
        if number in self.edited or self.previews[number - 1] is not None:
            return
        page, original = self.page_refs[number - 1], self._originals[number - 1]
        if (self.pages.getCount() != self.count
                or self.pages.getByIndex(number - 1) != page
                or page.Name != f'PDF source page {number}'
                or page.getCount() != 1 or page.getByIndex(0) != original):
            raise RuntimeError(f'Unprepared PDF page {number} was changed; refusing to replace objects')
        with self.timing.span('previews.prepare_page', page=number):
            # Keep the established owned-preview route and typed XGraphic
            # assignment. Reusing importer-owned shapes previously caused
            # wrapper/lifecycle failures; lazy preparation does not change it.
            graphic = original.getPropertyValue('Graphic')
            if graphic is None:
                raise RuntimeError(f'Internal PDF preview page {number} has no XGraphic')
            position, size = original.getPosition(), original.getSize()
            undo = self.document.getUndoManager()
            was_modified = self.document.isModified()
            preview = self.document.createInstance('com.sun.star.drawing.GraphicObjectShape')
            undo.lock()
            try:
                try:
                    page.add(preview)
                    set_graphic(preview, graphic)
                    preview.setPosition(position)
                    preview.setSize(size)
                    preview.setName(f'PDF reader preview {number}')
                    preview.setPropertyValue('MoveProtect', True)
                    preview.setPropertyValue('SizeProtect', True)
                    self.document.getLayerManager().attachShapeToLayer(preview, self.preview_layer)
                    page.remove(original)  # Commit only once replacement is ready.
                except BaseException:
                    # add() can throw after insertion. Remove only our object.
                    for i in range(page.getCount() - 1, -1, -1):
                        if page.getByIndex(i) == preview:
                            page.remove(preview)
                    raise
                self.previews[number - 1] = preview
                self._originals[number - 1] = None
            finally:
                self.document.setModified(was_modified)
                undo.unlock()

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
                self.ensure_preview(number)
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
        self.layer_manager.attachShapeToLayer(shape, self.edit_layer)
        if shape.supportsService('com.sun.star.drawing.GroupShape'):
            for i in range(shape.getCount()):
                self._attach_edit_layer(shape.getByIndex(i))

    def _reading_preview(self, page_number, *, phase='before conversion'):
        self.ensure_preview(page_number)
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

    def prefetch_page(self, page_number, anchor):
        """Synchronous idle preparation; never inserts into the user's document."""
        if self.direct_edit:
            return None  # Do not recreate temporary Draw documents for speculation.
        if self.document.isReadonly() or self.current_page() != anchor:
            return None
        neighbours = {p for p in (anchor + 1, anchor - 1) if 1 <= p <= self.count}
        self.staged_cache.retain(neighbours)
        if page_number not in neighbours or page_number in self.edited or page_number in self.staged_cache:
            return None
        started = time.monotonic()
        self.import_calls.append(page_number)
        staged = self.importer(self.context, self.pdf_url, hidden=True, page=page_number)
        try:
            if self.current_page() != anchor:
                return {'event': 'prefetch-cancelled', 'page': page_number}
            pages = staged.getDrawPages()
            if pages.getCount() != 1:
                raise RuntimeError('Prefetch did not return exactly one page')
            shapes = pages.getByIndex(0).getCount()
            owned, staged = staged, None
            accepted = self.staged_cache.put(page_number, owned, shapes)
            return {'event': 'prefetch-ready' if accepted else 'prefetch-budget-skipped',
                    'page': page_number, 'shapes': shapes,
                    'import_seconds': time.monotonic() - started}
        finally:
            if staged is not None:
                staged.dispose()

    def _copy_edit_shapes(self, shapes, page, preview, page_number, profile):
        """Copy in short view-update batches, retaining the preview until commit."""
        undo = self.document.getUndoManager()
        was_modified = self.document.isModified()
        added = []
        undo.lock()
        try:
            try:
                completed = 0
                with profile.phase('navigation'):
                    if self.current_page() != page_number:
                        raise ConversionCancelled('active-page-changed')
                    controller = self.document.getCurrentController()
                    if controller is None:
                        raise SessionClosed('PDF window no longer has a controller')
                while completed < len(shapes):
                    with profile.phase('navigation'):
                        if (controller.getCurrentPage() != page
                                or self.pages.getCount() != self.count
                                or page.Name != f'PDF source page {page_number}'):
                            raise ConversionCancelled('active-page-changed')
                    batch_started = time.monotonic()
                    batch_begin = completed
                    batch_end = min(completed + COPY_BATCH_SIZE, len(shapes))
                    with controller_batch(self.document, profile):
                        while completed < batch_end:
                            # Keep cancellation responsive without repeating all
                            # page identities and six layer checks per object.
                            with profile.phase('navigation'):
                                if controller.getCurrentPage() != page:
                                    raise ConversionCancelled('active-page-changed')
                            shape = shapes[completed]
                            with profile.phase('geometry_seed'):
                                # add() rebinds the source UNO wrapper. Seed its
                                # cached geometry before SvxShape::Create uses it.
                                position, size = shape.getPosition(), shape.getSize()
                                shape.setPosition(position)
                                shape.setSize(size)
                            with profile.phase('target_validation'):
                                old_count = page.getCount()
                                if old_count != len(added) + 1:
                                    raise RuntimeError('Target page changed during object copy')
                            try:
                                with profile.phase('native_add'):
                                    page.add(shape)
                            except BaseException:
                                # add() can throw after insertion: record the
                                # inserted clone even on that failure path.
                                with profile.phase('target_validation'):
                                    new_count = page.getCount()
                                    added.extend(page.getByIndex(i)
                                                 for i in range(old_count, new_count))
                                raise
                            with profile.phase('target_validation'):
                                new_count = page.getCount()
                                # SvxDrawPage::add rebinds this very UNO wrapper
                                # to the inserted clone. Avoid fetching it again.
                                if new_count == old_count + 1:
                                    added.append(shape)
                                else:
                                    added.extend(page.getByIndex(i)
                                                 for i in range(old_count, new_count))
                                if new_count != old_count + 1:
                                    raise RuntimeError('Object cloning did not insert exactly one shape')
                                actual_position, actual_size = added[-1].getPosition(), added[-1].getSize()
                                expected = (position.X, position.Y, size.Width, size.Height)
                                actual = (actual_position.X, actual_position.Y,
                                          actual_size.Width, actual_size.Height)
                                if any(abs(a - b) > 1 for a, b in zip(actual, expected)):
                                    raise RuntimeError(f'Cloned object geometry changed: expected={expected}, actual={actual}')
                            with profile.phase('layers'):
                                self._attach_edit_layer(added[-1])
                            completed += 1
                            if (completed - batch_begin >= COPY_BATCH_MIN_SIZE
                                    and time.monotonic() - batch_started >= COPY_BATCH_SECONDS):
                                break
                    # Release the model lock between batches for progressive
                    # display and navigation. A single native add cannot yield.
                    profile.progress(completed, len(shapes))
                with profile.phase('commit'):
                    if page.getCount() != len(shapes) + 1 or page.getByIndex(0) != preview:
                        raise RuntimeError('Object cloning did not preserve page contents')
                    if self.current_page() != page_number:
                        raise ConversionCancelled('active-page-changed')
                    page.remove(preview)
            except BaseException:
                with profile.phase('rollback'):
                    with controller_batch(self.document, profile):
                        for shape in reversed(added):
                            page.remove(shape)
                        self.document.setModified(was_modified)
                raise
        finally:
            undo.unlock()

    def edit_current_page(self):
        if self.document.isReadonly():
            raise RuntimeError('Enter document edit mode before converting pages')
        page_number = self.current_page()
        if page_number in self.edited:
            return {'page': page_number, 'reused': True}
        if self.direct_edit:
            return self._edit_direct_page(page_number)
        page = self.pages.getByIndex(page_number - 1)
        preview = self._reading_preview(page_number)
        # Conversion is staged before touching the reading document. Only the
        # requested source page may reach the edit converter's displayPage().
        started = time.monotonic()
        staged = self.staged_cache.take(page_number)
        cache_hit = staged is not None
        self.staged_cache.retain({page_number - 1, page_number + 1})
        if staged is None:
            self.import_calls.append(page_number)
            staged = self.importer(self.context, self.pdf_url, hidden=True, page=page_number)
        imported_at = time.monotonic()
        profile = CopyProfile(self.timing, page_number)
        status = 'failed'
        shapes = []
        try:
            with profile.phase('navigation'):
                if self.current_page() != page_number:
                    raise ConversionCancelled('active-page-changed')
                self._reading_preview(page_number, phase='after staged import')
            with profile.phase('source_enumeration'):
                imported = staged.getDrawPages()
                if imported.getCount() != 1:
                    raise RuntimeError('Selected-page import returned more than one page')
                source = imported.getByIndex(0)
                if abs(source.Width - page.Width) > 30 or abs(source.Height - page.Height) > 30:
                    raise RuntimeError('Imported geometry differs from reading page; crop/rotation needs adaptation')
                shapes = [source.getByIndex(i) for i in range(source.getCount())]
            self._copy_edit_shapes(shapes, page, preview, page_number, profile)
            self.edited.add(page_number)
            self.document.setModified(True)
            status = 'completed'
            return {'page': page_number, 'reused': False, 'shapes': len(shapes),
                    'geometry_verified': True,
                    'cache_hit': cache_hit,
                    'import_seconds': imported_at - started,
                    'copy_seconds': time.monotonic() - imported_at,
                    'copy_profile': profile.snapshot(),
                    'view': self.view_state()}
        except ConversionCancelled:
            status = 'cancelled'
            return {'page': page_number, 'cancelled': True,
                    'reason': 'active-page-changed', 'preview_retained': True,
                    'cache_hit': cache_hit, 'import_seconds': imported_at - started,
                    'copy_seconds': time.monotonic() - imported_at}
        finally:
            try:
                with profile.phase('staged_dispose'):
                    staged.dispose()
            finally:
                self.timing.mark('edit.copy.summary', page=page_number, status=status,
                                 shapes=len(shapes),
                                 duration_ms=round((time.monotonic() - imported_at) * 1000, 2),
                                 **profile.snapshot())

    def _edit_direct_page(self, number):
        page = self.pages.getByIndex(number - 1)
        preview = self._reading_preview(number)
        controller = self.document.getCurrentController()
        undo = self.document.getUndoManager()
        modified = self.document.isModified()
        prefix = 'PDF-reader-' + uuid.uuid4().hex + '-'
        profile = CopyProfile(self.timing, number)
        status = 'failed'
        started = time.monotonic()
        count = 0
        self.import_calls.append(number)
        undo.lock()
        try:
            with controller_batch(self.document, profile):
                try:
                    with profile.phase('direct_import'), self.timing.span('edit.direct.import', page=number):
                        import_into_page(self.context, self.document, page, self.pdf_url,
                                         number, self.edit_layer.Name, prefix)
                    with profile.phase('commit'):
                        count = page.getCount() - 1
                        if (self.pages.getCount() != self.count
                                or self.pages.getByIndex(number - 1) != page
                                or page.Name != f'PDF source page {number}'
                                or self.previews[number - 1] != preview
                                or count < 0):
                            raise RuntimeError('Direct PDF import changed target page identity')
                        # Native SAX is synchronous: navigation cancellation is
                        # checked before commit, not between imported shapes.
                        if controller is None or controller.getCurrentPage() != page:
                            raise ConversionCancelled('active-page-changed')
                        self.document.setModified(True)
                        page.remove(preview)
                        self.edited.add(number)
                        status = 'completed'
                except BaseException:
                    with profile.phase('rollback'):
                        # Only failure walks objects across UNO. The target was
                        # verified to contain exactly our owned preview before
                        # entering the native content-only importer.
                        preview_present = False
                        for index in range(page.getCount() - 1, -1, -1):
                            shape = page.getByIndex(index)
                            if shape != preview:
                                page.remove(shape)
                            else:
                                preview_present = True
                        if not preview_present:
                            page.add(preview)
                        discard_resources(self.context, self.document, prefix)
                        self.document.setModified(modified)
                    raise
            return {'page': number, 'reused': False, 'shapes': count,
                    'edit_route': 'native-direct-page', 'temporary_draw_documents': 0,
                    'copied_shapes': 0, 'page_geometry_verified': True,
                    'import_seconds': time.monotonic() - started,
                    'copy_seconds': 0.0, 'view': self.view_state()}
        except ConversionCancelled:
            status = 'cancelled'
            return {'page': number, 'cancelled': True, 'preview_retained': True,
                    'edit_route': 'native-direct-page', 'copy_seconds': 0.0}
        finally:
            undo.unlock()
            self.timing.mark('edit.direct.summary', page=number, status=status, shapes=count,
                             duration_ms=round((time.monotonic() - started) * 1000, 2),
                             **profile.snapshot())

    def close(self):
        try:
            self.staged_cache.close()
        finally:
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
