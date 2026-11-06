#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Three real Draw pages and a live PageShape overview, NOT inline page editing.

The overview is a fourth, synthetic SdPage. Commands switch the same Draw
controller between that overview and the real editable second page. PDF page
1/3 previews still use only Office's integrated PDFium. Page 2 is a fixture,
not an editable import of the source PDF.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import tempfile

from p0a_draw_host_probe import add_shape, point, prop, size
from p0a_preview_probe import run


class PageGeometryUnsupported(RuntimeError):
    def __init__(self, sizes, overview_size):
        super().__init__('Resizing the overview also resized content pages')
        self.report = {
            'status': 'unsupported-page-geometry',
            'reason': str(self),
            'expected_content_page_sizes': [[20000, 10000]] * 3,
            'actual_content_page_sizes': sizes,
            'overview_size': overview_size,
            'multi_page_layout_verified': False,
            'inline_editing_verified': False,
        }


def overview_png(remote, page, destination):
    exporter = remote.ServiceManager.createInstanceWithContext(
        'com.sun.star.drawing.GraphicExportFilter', remote)
    exporter.setSourceDocument(page)
    if not exporter.filter((prop('URL', destination.as_uri()),
                            prop('MediaType', 'image/png'),
                            prop('FilterData', (prop('PixelWidth', 600),
                                                prop('PixelHeight', 1100))))):
        raise RuntimeError('Cannot render the live page overview')
    data = destination.read_bytes()
    if not data.startswith(b'\x89PNG\r\n\x1a\n'):
        raise RuntimeError('Overview export is not PNG')
    return hashlib.sha256(data).hexdigest()


def build(remote, images):
    desktop = remote.ServiceManager.createInstanceWithContext(
        'com.sun.star.frame.Desktop', remote)
    doc = desktop.loadComponentFromURL('private:factory/sdraw', '_blank', 0, ())
    if doc is None:
        raise RuntimeError('Cannot create Draw document')
    try:
        pages = doc.getDrawPages()
        first = pages.getByIndex(0)
        middle = pages.insertNewByIndex(1)
        last = pages.insertNewByIndex(2)
        overview = pages.insertNewByIndex(3)
        masters = doc.getMasterPages()
        overview_master = masters.insertNewByIndex(masters.getCount())
        overview_master.Name = 'PDF overview format'
        overview.setMasterPage(overview_master)
        for index, page in enumerate((first, middle, last), 1):
            page.Name = f'PDF page {index}'
            page.Width, page.Height = 20000, 10000
            page.BorderLeft = page.BorderRight = 0
            page.BorderTop = page.BorderBottom = 0
        overview.Name = 'Continuous overview (probe only)'
        overview.Width, overview.Height = 22000, 35000
        dimensions = [(page.Width, page.Height) for page in (first, middle, last)]
        if dimensions != [(20000, 10000)] * 3:
            raise PageGeometryUnsupported(dimensions, (overview.Width, overview.Height))
        layers = doc.getLayerManager()
        layer = layers.insertNewByIndex(layers.getCount())
        layer.Name = 'Locked page previews'
        layer.IsVisible, layer.IsPrintable = True, True
        provider = remote.ServiceManager.createInstanceWithContext(
            'com.sun.star.graphic.GraphicProvider', remote)
        for page, image in zip((first, last), images):
            graphic = provider.queryGraphic((prop('URL', image.as_uri()),))
            if graphic is None:
                raise RuntimeError('Cannot load internal PDFium preview')
            pixels = graphic.getPropertyValue('SizePixel')
            if pixels.Width <= 0 or pixels.Height <= 0:
                raise RuntimeError('Invalid preview dimensions')
            width = min(20000, 10000 * pixels.Width // pixels.Height)
            height = width * pixels.Height // pixels.Width
            shape = doc.createInstance('com.sun.star.drawing.GraphicObjectShape')
            page.add(shape)
            shape.Graphic = graphic
            shape.setPosition(point((20000 - width) // 2, (10000 - height) // 2))
            shape.setSize(size(width, height))
            layers.attachShapeToLayer(shape, layer)
        add_shape(doc, middle, 'com.sun.star.drawing.RectangleShape',
                  0, 0, 20000, 10000, color=0xFFF4D6)
        text = add_shape(doc, middle, 'com.sun.star.drawing.TextShape',
                         1000, 2000, 18000, 6000,
                         'EDITABLE PAGE 2\nChange this text, move it, undo and redo.')
        text.Name = 'P0A editable page 2 text'
        references = []
        for index in range(3):
            reference = doc.createInstance('com.sun.star.drawing.PageShape')
            overview.add(reference)
            reference.PageNumber = index + 1  # UNO page numbers are one-based.
            reference.setPosition(point(1000, 1000 + index * 11000))
            reference.setSize(size(20000, 10000))
            layers.attachShapeToLayer(reference, layer)
            references.append(reference)
        layer.IsLocked = True
        if [ref.PageNumber for ref in references] != [1, 2, 3]:
            raise RuntimeError('PageShape references do not point to the real pages')
        controller = doc.getCurrentController()
        controller.setCurrentPage(middle)
        if controller.getCurrentPage().Name != middle.Name:
            raise RuntimeError('Cannot activate the real edit page')
        controller.select(text)
        controller.setCurrentPage(overview)
        if controller.getCurrentPage().Name != overview.Name:
            raise RuntimeError('Cannot return to overview')

        # Exercise real rendering, not just the reference property: a change on
        # page 2 must alter the overview WITHOUT replacing its PageShapes.
        with tempfile.TemporaryDirectory(prefix='p0a-reference-check-') as tmp:
            directory = Path(tmp)
            original = text.getString()
            before = overview_png(remote, overview, directory / 'before.png')
            text.setString('CHANGED PAGE 2\nLIVE REFERENCE UPDATE VERIFIED')
            changed = overview_png(remote, overview, directory / 'changed.png')
            text.setString(original)
            restored = overview_png(remote, overview, directory / 'restored.png')
        if before == changed or before != restored:
            raise RuntimeError('Live overview did not reflect and restore the page edit')
        doc.getUndoManager().clear()  # Remove fixture setup; manual edits start here.
        doc.setModified(False)
        return doc, desktop, {
            'status': 'structural-checks-passed',
            'draw_pages': pages.getCount(), 'content_pages': 3,
            'content_page_sizes': dimensions,
            'overview_size': [overview.Width, overview.Height],
            'synthetic_overview_pages': 1, 'page_references': [1, 2, 3],
            'preview_images': 2, 'preview_layer_locked': bool(layer.IsLocked),
            'edit_page_switch_verified': True,
            'live_reference_render_changed': before != changed,
            'live_reference_render_restored': before == restored,
            'overview_render_sha256': [before, changed, restored],
            'multi_page_layout_verified': False,
            'inline_editing_verified': False,
            'specified_pdf_page_edit_import_verified': False,
            'renderer': 'Office internal PDFium previews; Draw PageShape overview',
        }
    except BaseException:
        doc.dispose()
        raise


def interact(doc):
    controller = doc.getCurrentController()
    pages = doc.getDrawPages()
    states = {}
    print('Overview is synthetic page 4; real editable page is page 2.', flush=True)
    while True:
        command = input('e = edit page 2; o = overview; q or Enter = close: ').strip().lower()
        if command in ('', 'q'):
            return
        if command not in ('e', 'o'):
            print('Use e, o or q.', flush=True)
            continue
        current = controller.getCurrentPage().Name
        offset = controller.ViewOffset
        states[current] = (controller.ZoomValue, offset.X, offset.Y)
        target = pages.getByIndex(1 if command == 'e' else 3)
        controller.setCurrentPage(target)
        if target.Name in states:
            zoom, x, y = states[target.Name]
            controller.ZoomValue = zoom
            controller.ViewOffset = point(x, y)
        print(f'Active page: {controller.getCurrentPage().Name}', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--office', default='libreoffice')
    parser.add_argument('--pdf', help='Optional PDF with at least three pages')
    parser.add_argument('--save', help='New .odg fixture, not a PDF saving implementation')
    parser.add_argument('--report', help='New JSON report path')
    parser.add_argument('--headless', action='store_true', help='Automatic checks only')
    args = parser.parse_args()
    try:
        run(args, builder=build, interaction=interact)
    except PageGeometryUnsupported as error:
        print(json.dumps(error.report, indent=2), flush=True)
        if args.report:
            with Path(args.report).open('x', encoding='utf-8') as output:
                json.dump(error.report, output, indent=2)
        sys.exit(2)
