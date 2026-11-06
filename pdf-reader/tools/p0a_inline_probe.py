#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""P0-A1: requires the patched Draw inline host; never simulates it in a tall page."""
import argparse
import json
from pathlib import Path
import tempfile
import time

from p0a_draw_host_probe import add_shape, point, prop, size
from p0a_preview_probe import run


def build(remote, images, ready_path, mixed_sizes=False):
    desktop = remote.ServiceManager.createInstanceWithContext('com.sun.star.frame.Desktop', remote)
    doc = desktop.loadComponentFromURL('private:factory/sdraw', '_blank', 0, ())
    if doc is None:
        raise RuntimeError('Cannot create Draw fixture')
    try:
        pages = doc.getDrawPages()
        pages.insertNewByIndex(1)
        pages.insertNewByIndex(2)
        names = ['P0A1 reading before', 'P0A1 active edit', 'P0A1 reading after']
        dimensions = [(18000, 9000), (20000, 10000), (24000, 12000)] if mixed_sizes else [(20000, 10000)] * 3
        # Each page gets its own master to avoid size propagation in 26.2.6.
        masters = doc.getMasterPages()
        for index in range(3):
            page = pages.getByIndex(index)
            if index:
                master = masters.insertNewByIndex(masters.getCount())
                master.Name = f'P0A1 format {index + 1}'
                page.setMasterPage(master)
            page.Width, page.Height = dimensions[index]
            page.BorderLeft = page.BorderRight = 0
            page.BorderTop = page.BorderBottom = 0
        actual = [(pages.getByIndex(i).Width, pages.getByIndex(i).Height) for i in range(3)]
        if actual != dimensions:
            raise RuntimeError(f'Page sizes changed unexpectedly: {actual}; expected {dimensions}')
        layers = doc.getLayerManager()
        layer = layers.insertNewByIndex(layers.getCount())
        layer.Name = 'P0A1 locked PDF previews'
        layer.IsVisible, layer.IsPrintable = True, True
        provider = remote.ServiceManager.createInstanceWithContext('com.sun.star.graphic.GraphicProvider', remote)
        for index, image in zip((0, 2), images):
            page = pages.getByIndex(index)
            graphic = provider.queryGraphic((prop('URL', image.as_uri()),))
            pixels = graphic.getPropertyValue('SizePixel')
            if pixels.Width <= 0 or pixels.Height <= 0:
                raise RuntimeError('Invalid internal PDFium preview')
            shape = doc.createInstance('com.sun.star.drawing.GraphicObjectShape')
            page.add(shape)
            shape.Graphic = graphic
            shape.setPosition(point(0, 0))
            shape.setSize(size(page.Width, page.Height))
            layers.attachShapeToLayer(shape, layer)
        layer.IsLocked = True
        middle = pages.getByIndex(1)
        add_shape(doc, middle, 'com.sun.star.drawing.RectangleShape',
                  0, 0, middle.Width, middle.Height, color=0xFFF4D6)
        text = add_shape(doc, middle, 'com.sun.star.drawing.TextShape', 1000, 2000, 18000, 5000,
                         'INLINE EDIT PAGE 2\nEdit this text while both PDF neighbours remain visible.')
        # Names are the final opt-in fixture marker. Publish only after setup.
        for index in range(3):
            pages.getByIndex(index).Name = names[index]
        controller = doc.getCurrentController()
        controller.setCurrentPage(middle)
        controller.select(text)
        controller.ZoomValue = 45
        doc.getUndoManager().clear()  # Fixture setup only, before any user edit.
        doc.setModified(False)
        deadline = time.monotonic() + 12
        hook = None
        while time.monotonic() < deadline:
            # Give the GUI event loop opportunities to attach the view overlays.
            controller.getCurrentPage()
            try:
                hook = json.loads(ready_path.read_text(encoding='utf-8'))
                break
            except (FileNotFoundError, json.JSONDecodeError):
                time.sleep(0.1)
        if not hook or hook.get('status') != 'host-attached' or hook.get('overlays') != 2:
            raise RuntimeError('No native inline-host handshake. Use this branch\'s rebuilt '
                               'GUI Office (instdir/program/soffice), not the previous installed package.')
        if hook.get('active_page_origin_x') != 0 or hook.get('active_page_origin_y') != 0:
            raise RuntimeError('The native edit page origin changed')
        if hook.get('before_y') != -dimensions[0][1] - 1000 or hook.get('after_y') != dimensions[1][1] + 1000:
            raise RuntimeError('Native neighbour positions do not match the real page sizes')
        return doc, desktop, {
            'status': 'native-host-attached-gui-validation-pending',
            'draw_pages': pages.getCount(), 'synthetic_overview_pages': 0,
            'content_page_sizes': actual, 'native_host': hook,
            'preview_images': 2, 'preview_layer_locked': bool(layer.IsLocked),
            'native_text_verified': 'INLINE EDIT PAGE 2' in text.getString(),
            'multi_page_layout_verified': False, 'inline_editing_verified': False,
            'specified_pdf_page_edit_import_verified': False,
            'renderer': 'Office internal PDFium images in non-hittable Draw view overlays',
        }
    except BaseException:
        doc.dispose()
        raise


def interact(doc):
    controller = doc.getCurrentController()
    middle = doc.getDrawPages().getByIndex(1)
    print('Do not switch pages: scroll through the neighbours and edit real page 2 in place.', flush=True)
    input('Check text/caret, selection, move, undo/redo, zoom and locked previews. Enter closes: ')
    if controller.getCurrentPage().Name != middle.Name:
        print('The active page was changed; this run does not validate continuous inline editing.', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--office', default='libreoffice')
    parser.add_argument('--report', help='New JSON file; records attachment, not a GUI pass')
    parser.add_argument('--mixed-sizes', action='store_true', help='Also exercise distinct page sizes')
    args = parser.parse_args()
    args.pdf = args.save = None
    args.headless = False  # This experiment needs an actual window/overlay manager.
    with tempfile.TemporaryDirectory(prefix='p0a-inline-hook-') as tmp:
        ready_path = Path(tmp) / 'ready.json'
        run(args, builder=lambda remote, images: build(remote, images, ready_path, args.mixed_sizes),
            interaction=interact, office_env={
                'LO_PDF_HYBRID_VIEW_PROBE': '1',
                'LO_PDF_HYBRID_VIEW_PROBE_REPORT': str(ready_path),
            })
