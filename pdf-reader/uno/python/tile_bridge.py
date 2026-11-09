# SPDX-License-Identifier: MPL-2.0
"""Experimental internal PDFium tile adapter; no external rasterizer."""
from pathlib import Path
import struct
from edit_bridge import property_value as prop


class TileBridgeUnavailable(RuntimeError):
    pass


def validate_tile(page_pixels, rect):
    width, height = page_pixels
    x, y, w, h = rect
    if (any(type(v) is not int for v in (*page_pixels, *rect))
            or not 0 < width <= 131072 or not 0 < height <= 131072
            or not 0 < w <= 2048 or not 0 < h <= 2048
            or x < 0 or y < 0 or x + w > width or y + h > height):
        raise ValueError('Invalid PDF tile raster bounds')


def tile_placement(page_pixels, rect, bounds):
    """Map shared raster edges to identical Draw coordinates (1/100 mm)."""
    validate_tile(page_pixels, rect)
    width, height = page_pixels
    x, y, w, h = rect
    left, top, dw, dh = bounds
    x1, x2 = round(x * dw / width), round((x + w) * dw / width)
    y1, y2 = round(y * dh / height), round((y + h) * dh / height)
    return (left + x1, top + y1, x2 - x1, y2 - y1)


class InternalTileRenderer:
    def __init__(self, context):
        self.provider = context.ServiceManager.createInstanceWithContext(
            'com.sun.star.graphic.GraphicProvider', context)
        self.checked = False

    def _export(self, graphic, path, page_pixels, rect):
        import uno
        size = uno.createUnoStruct('com.sun.star.awt.Size')
        size.Width, size.Height = page_pixels
        area = uno.createUnoStruct('com.sun.star.awt.Rectangle')
        area.X, area.Y, area.Width, area.Height = rect
        self.provider.storeGraphic(graphic, (prop('URL', Path(path).resolve().as_uri()),
            prop('MimeType', 'image/png'),
            prop('FilterData', (prop('PdfTilePageSize', size), prop('PdfTileRect', area)))))

    def render(self, graphic, path, page_pixels, rect):
        import uno
        validate_tile(page_pixels, rect)
        path = Path(path)
        if path.exists():
            raise FileExistsError(path)
        if not self.checked:
            # Old providers silently ignore unknown FilterData. Require the
            # bridge's explicit rejection of an invalid zero-width request.
            check = path.with_name(path.name + '.capability.png')
            if check.exists():
                raise FileExistsError(check)
            try:
                try:
                    self._export(graphic, check, page_pixels, (0, 0, 0, 1))
                except uno.getClass('com.sun.star.lang.IllegalArgumentException') as error:
                    if 'PDF tile:' not in str(error):
                        raise
                    self.checked = True
                else:
                    raise TileBridgeUnavailable('Rebuild Office with the internal PDF tile bridge')
            finally:
                check.unlink(missing_ok=True)
        self._export(graphic, path, page_pixels, rect)
        header = path.read_bytes()[:24]
        if (header[:8] != b'\x89PNG\r\n\x1a\n' or len(header) < 24
                or struct.unpack('>II', header[16:24]) != tuple(rect[2:])):
            raise RuntimeError('Internal tile output dimensions do not match the request')
        return path
