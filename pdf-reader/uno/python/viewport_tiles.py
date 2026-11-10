# SPDX-License-Identifier: MPL-2.0
"""Bounded, synchronous visible-tile reader using Office's PDFium bridge.

All UNO calls stay on the reader loop. Native rendering is not interruptible;
navigation is checked again before publishing each completed tile.
"""
from collections import OrderedDict
from contextlib import contextmanager
from dataclasses import dataclass
import math
from pathlib import Path
import uuid

from edit_bridge import property_value as prop
from reader_session import set_graphic
from tile_bridge import InternalTileRenderer, TileBridgeUnavailable, tile_placement


@dataclass(frozen=True)
class TilePlan:
    page: int
    pixels: tuple
    bounds: tuple
    rects: tuple


def plan_tiles(page, bounds, visible, zoom, dpi=96, *, tile_size=512, limit=64):
    """Draw coordinates are 1/100 mm; prioritize tiles nearest the viewport centre."""
    if (not math.isfinite(zoom) or not math.isfinite(dpi) or zoom <= 0 or dpi <= 0
            or tile_size <= 0 or tile_size > 2048 or limit <= 0):
        raise ValueError('Invalid viewport scale or tile budget')
    left, top, width, height = bounds
    vx, vy, vw, vh = visible
    if width <= 0 or height <= 0:
        raise ValueError('Empty PDF preview bounds')
    # Render at 1.5 device pixels per screen pixel to retain antialiasing detail.
    scale = zoom / 100 * dpi / 2540 * 1.5
    scale = min(scale, 131072 / max(width, height))
    pixels = (max(1, math.ceil(width * scale)), max(1, math.ceil(height * scale)))
    x1, y1 = max(left, vx), max(top, vy)
    x2, y2 = min(left + width, vx + vw), min(top + height, vy + vh)
    if x2 <= x1 or y2 <= y1:
        return TilePlan(page, pixels, bounds, ())
    pw, ph = pixels
    rx1, ry1 = (x1 - left) * pw / width, (y1 - top) * ph / height
    rx2, ry2 = (x2 - left) * pw / width, (y2 - top) * ph / height
    cx, cy = (rx1 + rx2) / 2, (ry1 + ry2) / 2
    rects = [(x, y, min(tile_size, pw - x), min(tile_size, ph - y))
             for y in range(int(ry1) // tile_size * tile_size, math.ceil(ry2), tile_size)
             for x in range(int(rx1) // tile_size * tile_size, math.ceil(rx2), tile_size)]
    rects.sort(key=lambda r: ((r[0] + r[2] / 2 - cx) ** 2
                              + (r[1] + r[3] / 2 - cy) ** 2, r[1], r[0]))
    return TilePlan(page, pixels, bounds, tuple(rects[:limit]))


class TileCache:
    """PNG file LRU with a decoded-pixel budget, not an unbounded UNO object cache."""
    def __init__(self, directory, budget=64 * 1024 * 1024):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.budget, self.bytes = budget, 0
        self.items = OrderedDict()

    def get(self, key):
        entry = self.items.get(key)
        if entry:
            self.items.move_to_end(key)
            return entry[0]

    def put(self, key, path, cost):
        if cost > self.budget:
            raise ValueError('Tile exceeds cache budget')
        previous = self.items.pop(key, None)
        if previous:
            self.bytes -= previous[1]
            if previous[0] != path:
                previous[0].unlink(missing_ok=True)
        self.items[key] = (path, cost)
        self.bytes += cost
        while self.bytes > self.budget:
            _, (old, size) = self.items.popitem(last=False)
            self.bytes -= size
            old.unlink(missing_ok=True)

    def close(self):
        for path, _ in self.items.values():
            path.unlink(missing_ok=True)
        self.items.clear()
        self.bytes = 0


class ViewportTiles:
    def __init__(self, session, directory, *, renderer=None, settle=.18):
        self.session = session
        self.renderer = renderer or InternalTileRenderer(session.context)
        self.cache = TileCache(directory)
        self.settle = settle
        self.plan = None
        self.since = 0
        self.shapes = {}
        self.disabled = False
        self.error = None
        self.metrics = {'rendered': 0, 'published': 0, 'cache_hits': 0, 'stale_discarded': 0,
                        'peak_visible_tiles': 0, 'peak_cache_bytes': 0,
                        'reading_mode_only': True, 'native_render_cancellable': False}

    def snapshot(self):
        number = self.session.current_page()
        controller = self.session.document.getCurrentController()
        preview = self.session.previews[number - 1]
        position, size = preview.getPosition(), preview.getSize()
        area = controller.getPropertyValue('VisibleArea')
        offset = controller.getPropertyValue('ViewOffset')
        zoom = controller.getPropertyValue('ZoomValue')
        # ViewOffset is explicitly page-relative; VisibleArea supplies its extent.
        dpi = 96
        try:
            info = controller.getFrame().getComponentWindow().getInfo()
            if info.PixelPerMeterX > 0:
                dpi = info.PixelPerMeterX * .0254
        except Exception:
            pass  # Backends without XDevice use the conventional 96 DPI scale.
        return plan_tiles(number, (position.X, position.Y, size.Width, size.Height),
                          (offset.X, offset.Y, area.Width, area.Height), zoom, dpi)

    @contextmanager
    def overlay_update(self):
        document = self.session.document
        modified = document.isModified()
        undo = document.getUndoManager()
        undo.lock()
        try:
            yield
        finally:
            try:
                document.setModified(modified)
            finally:
                undo.unlock()

    def remove_except(self, wanted):
        obsolete = set(self.shapes) - wanted
        if obsolete:
            with self.overlay_update():
                for key in obsolete:
                    page, shape = self.shapes[key]
                    page.remove(shape)
                    del self.shapes[key]

    def clear(self):
        self.remove_except(set())
        self.plan = None

    def tick(self, now):
        if self.disabled:
            return None
        plan = self.snapshot()
        keys = {(plan.page, plan.pixels, rect) for rect in plan.rects}
        if plan != self.plan:
            self.remove_except(keys)
            self.plan, self.since = plan, now
            return None
        if now - self.since < self.settle:
            return None
        for rect in plan.rects:
            key = (plan.page, plan.pixels, rect)
            if key in self.shapes:
                continue
            path = self.cache.get(key)
            cached = path is not None
            if not cached:
                path = self.cache.directory / (uuid.uuid4().hex + '.png')
                try:
                    self.renderer.render(self.session.previews[plan.page - 1].getPropertyValue('Graphic'),
                                         path, plan.pixels, rect)
                except TileBridgeUnavailable as error:
                    path.unlink(missing_ok=True)
                    self.disabled, self.error = True, str(error)
                    return {'event': 'tiles-unavailable', 'error': self.error}
                except Exception as error:
                    path.unlink(missing_ok=True)
                    # Keep the reader usable, retaining its complete base preview.
                    self.disabled, self.error = True, str(error)
                    return {'event': 'tiles-failed', 'error': self.error}
                self.cache.put(key, path, rect[2] * rect[3] * 4)
                self.metrics['rendered'] += 1
            else:
                self.metrics['cache_hits'] += 1
            self.metrics['peak_cache_bytes'] = max(self.metrics['peak_cache_bytes'], self.cache.bytes)
            if self.snapshot() != plan:
                self.metrics['stale_discarded'] += 1
                return None
            try:
                self.install(key, plan, rect, path)
            except Exception as error:
                self.disabled, self.error = True, str(error)
                self.clear()
                return {'event': 'tiles-failed', 'error': self.error}
            self.metrics['published'] += 1
            self.metrics['peak_visible_tiles'] = max(self.metrics['peak_visible_tiles'], len(self.shapes))
            return {'event': 'reading-tile', 'page': plan.page, 'page_pixels': plan.pixels,
                    'rect': rect, 'cache_hit': cached, 'visible_tiles': len(self.shapes)}
        return None

    def install(self, key, plan, rect, path):
        import uno
        page = self.session.page_refs[plan.page - 1]
        graphic = self.renderer.provider.queryGraphic((prop('URL', path.resolve().as_uri()),))
        with self.overlay_update():
            shape = self.session.document.createInstance('com.sun.star.drawing.GraphicObjectShape')
            page.add(shape)
            try:
                set_graphic(shape, graphic)
                x, y, w, h = tile_placement(plan.pixels, rect, plan.bounds)
                shape.setPosition(uno.createUnoStruct('com.sun.star.awt.Point', x, y))
                shape.setSize(uno.createUnoStruct('com.sun.star.awt.Size', w, h))
                shape.setName('PDF reader temporary tile')
                shape.MoveProtect = shape.SizeProtect = True
                self.session.document.getLayerManager().attachShapeToLayer(shape, self.session.preview_layer)
                self.shapes[key] = (page, shape)
            except BaseException:
                page.remove(shape)
                raise

    def close(self):
        try:
            self.clear()
        finally:
            self.cache.close()
