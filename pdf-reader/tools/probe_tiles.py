#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Minimal PDFium tile/Draw placement experiment, separate from the reader UI."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'uno' / 'python'))
from edit_bridge import property_value as prop
from reader_session import ReaderSession, set_graphic
from tile_bridge import InternalTileRenderer, TileBridgeUnavailable, tile_placement
from probe_edit import write_fixture


def run(args):
    import uno
    output = Path(args.out).resolve()
    output.mkdir(parents=True, exist_ok=False)
    source = output / 'fixture.pdf'
    write_fixture(source)
    pipe = 'pdf_tile_' + uuid.uuid4().hex
    command = [args.office, '-env:UserInstallation=' + (output / 'profile').as_uri(),
               '--nologo', '--nodefault', '--nofirststartwizard', '--norestore',
               f'--accept=pipe,name={pipe};urp;StarOffice.ComponentContext']
    if not args.wait:
        command.append('--headless')
    env = os.environ.copy()
    env['LO_IMPORT_USE_PDFIUM'] = '1'
    env.pop('LO_PDF_HYBRID_VIEW_PROBE', None)
    session = desktop = None
    result = {'status': 'started', 'external_renderer_used': False,
              'automatic_zoom_tiles': False, 'viewport_scheduler': False}
    with (output / 'office.log').open('w') as log:
        process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            local = uno.getComponentContext()
            resolver = local.ServiceManager.createInstanceWithContext('com.sun.star.bridge.UnoUrlResolver', local)
            deadline = time.monotonic() + 30
            while True:
                try:
                    context = resolver.resolve(f'uno:pipe,name={pipe};urp;StarOffice.ComponentContext')
                    break
                except Exception:
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError('Cannot connect to private Office')
                    time.sleep(.1)
            desktop = context.ServiceManager.createInstanceWithContext('com.sun.star.frame.Desktop', context)
            session = ReaderSession(context, source.as_uri(), hidden=not args.wait)
            renderer = InternalTileRenderer(context)
            graphic = session.previews[0].getPropertyValue('Graphic')
            page_pixels = (2048, 1024)
            rects = [(256, 128, 512, 512), (768, 128, 512, 512)]
            try:
                renderer.render(graphic, output / 'tile-0.png', page_pixels, rects[0])
            except TileBridgeUnavailable:
                result['status'] = 'unsupported'
                if not args.expect_unsupported:
                    raise
                return
            if args.expect_unsupported:
                raise RuntimeError('Bridge is available; unsupported check was not satisfied')
            renderer.render(graphic, output / 'tile-1.png', page_pixels, rects[1])
            renderer.render(graphic, output / 'reference.png', page_pixels, (0, 0, *page_pixels))
            # Pillow only decodes images for QA. All pixels originate in the
            # Office internal renderer, including this full-page reference.
            from PIL import Image, ImageChops, ImageStat
            reference = Image.open(output / 'reference.png').convert('RGB')
            for index, (x, y, w, h) in enumerate(rects):
                tile = Image.open(output / f'tile-{index}.png').convert('RGB')
                difference = ImageChops.difference(reference.crop((x, y, x + w, y + h)), tile)
                error = sum(ImageStat.Stat(difference).mean) / 3
                histogram = difference.histogram()
                large_errors = sum(sum(histogram[c * 256 + 17:(c + 1) * 256]) for c in range(3))
                if error >= .25 or large_errors / (w * h * 3) >= .001:
                    raise RuntimeError('Tile differs materially from same-resolution PDFium reference crop')
            page = session.pages.getByIndex(0)
            preview = session.previews[0]
            position, size = preview.getPosition(), preview.getSize()
            bounds = (position.X, position.Y, size.Width, size.Height)
            placements = []
            for index, rect in enumerate(rects):
                placement = tile_placement(page_pixels, rect, bounds)
                shape = session.document.createInstance('com.sun.star.drawing.GraphicObjectShape')
                page.add(shape)
                tile_graphic = renderer.provider.queryGraphic((prop('URL', (output / f'tile-{index}.png').as_uri()),))
                set_graphic(shape, tile_graphic)
                p, s = uno.createUnoStruct('com.sun.star.awt.Point'), uno.createUnoStruct('com.sun.star.awt.Size')
                p.X, p.Y, s.Width, s.Height = placement
                shape.setPosition(p)
                shape.setSize(s)
                shape.setName(f'Experimental PDF tile {index}')
                session.document.getLayerManager().attachShapeToLayer(shape, session.preview_layer)
                actual_p, actual_s = shape.getPosition(), shape.getSize()
                if (actual_p.X, actual_p.Y, actual_s.Width, actual_s.Height) != placement:
                    raise RuntimeError('Draw tile placement changed')
                placements.append(placement)
            if placements[0][0] + placements[0][2] != placements[1][0]:
                raise RuntimeError('Adjacent Draw tiles have a coordinate gap')
            result.update(status='passed', pixel_crop_verified=True, placement_verified=True,
                          placements=placements, preview_retained=page.getCount() == 3,
                          edited_imports=len(session.import_calls), page_pixels=page_pixels)
            session.document.setModified(False)
            if args.wait:
                print('Two fixed high-resolution tiles overlay the low-resolution preview. '
                      'Check zoom, alignment and seam; this is NOT automatic tile reading. Enter closes.', flush=True)
                input()
        except BaseException as error:
            result.update(status='failed', error=f'{type(error).__name__}: {error}')
            raise
        finally:
            (output / 'report.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
            print(json.dumps(result, indent=2), flush=True)
            try:
                if session:
                    session.close()
                if desktop:
                    desktop.terminate()
            finally:
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--office', default='libreoffice')
    parser.add_argument('--out', required=True, help='New evidence directory')
    parser.add_argument('--wait', action='store_true', help='Show the Draw experiment until Enter')
    parser.add_argument('--expect-unsupported', action='store_true', help='Check old builds fail closed')
    run(parser.parse_args())
    import gc
    gc.collect()
