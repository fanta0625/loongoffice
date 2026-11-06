#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Real PDF previews on a locked Draw layer, alongside native editable objects.

An intermediate host experiment, not a multi-SdPage reader implementation.
pdftoppm is an external prototype renderer, not the final PDFium backend.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import uuid

from p0a_draw_host_probe import add_shape, point, prop, size


def write_fixture(path):
    objects = [b'<< /Type /Catalog /Pages 2 0 R >>',
               b'<< /Type /Pages /Kids [4 0 R 6 0 R 8 0 R] /Count 3 >>',
               b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>']
    for index in range(3):
        stream_id = 5 + index * 2
        objects.append((f'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 200] '
                        f'/Resources << /Font << /F1 3 0 R >> >> /Contents {stream_id} 0 R >>').encode())
        content = (f'0.85 0.92 1 rg 0 0 400 200 re f '
                   f'0 0 0 rg BT /F1 22 Tf 25 140 Td (PDF SOURCE PAGE {index + 1}) Tj '
                   '0 -40 Td /F1 12 Tf (Real PDF preview - not editable Draw text) Tj ET\n').encode()
        objects.append(f'<< /Length {len(content)} >>\nstream\n'.encode() + content + b'endstream')
    data = bytearray(b'%PDF-1.4\n')
    offsets = []
    for index, obj in enumerate(objects, 1):
        offsets.append(len(data))
        data.extend(f'{index} 0 obj\n'.encode() + obj + b'\nendobj\n')
    xref = len(data)
    data.extend(f'xref\n0 {len(objects)+1}\n0000000000 65535 f \n'.encode())
    for offset in offsets:
        data.extend(f'{offset:010d} 00000 n \n'.encode())
    data.extend((f'trailer\n<< /Size {len(objects)+1} /Root 1 0 R >>\n'
                 f'startxref\n{xref}\n%%EOF\n').encode())
    path.write_bytes(data)


def render(pdf, directory):
    renderer = shutil.which('pdftoppm')
    if not renderer:
        raise RuntimeError('Install poppler-utils (pdftoppm) for this prototype renderer')
    images = []
    for page in (1, 3):
        prefix = directory / f'page-{page}'
        subprocess.run([renderer, '-f', str(page), '-l', str(page), '-singlefile',
                        '-scale-to', '1400', '-png', str(pdf), str(prefix)],
                       check=True, timeout=60, capture_output=True)
        image = prefix.with_suffix('.png')
        if not image.is_file():
            raise RuntimeError(f'PDF has no renderable page {page}')
        images.append(image)
    return images


def build(remote, images):
    services = remote.ServiceManager
    desktop = services.createInstanceWithContext('com.sun.star.frame.Desktop', remote)
    doc = desktop.loadComponentFromURL('private:factory/sdraw', '_blank', 0, ())
    if doc is None:
        raise RuntimeError('Cannot create Draw document')
    try:
        page = doc.getDrawPages().getByIndex(0)
        page.Width, page.Height = 20000, 30000
        layers = doc.getLayerManager()
        layer = layers.insertNewByIndex(layers.getCount())
        layer.Name = 'PDF reading previews'
        layer.IsVisible, layer.IsPrintable = True, True
        provider = services.createInstanceWithContext('com.sun.star.graphic.GraphicProvider', remote)
        for image, y in zip(images, (1000, 20000)):
            graphic = provider.queryGraphic((prop('URL', image.as_uri()),))
            if graphic is None:
                raise RuntimeError(f'Cannot load preview {image}')
            pixels = graphic.getPropertyValue('SizePixel')
            if pixels.Width <= 0 or pixels.Height <= 0:
                raise RuntimeError('Preview dimensions are invalid')
            width = min(18000, 8500 * pixels.Width // pixels.Height)
            height = width * pixels.Height // pixels.Width
            shape = doc.createInstance('com.sun.star.drawing.GraphicObjectShape')
            page.add(shape)
            shape.Graphic = graphic
            shape.setPosition(point(1000, y))
            shape.setSize(size(width, height))
            shape.MoveProtect, shape.SizeProtect = True, True
            layers.attachShapeToLayer(shape, layer)
        layer.IsLocked = True
        add_shape(doc, page, 'com.sun.star.drawing.RectangleShape',
                  1000, 10500, 18000, 8500, color=0xFFF4D6)
        text = add_shape(doc, page, 'com.sun.star.drawing.TextShape',
                         1800, 11500, 16000, 4000,
                         'EDITABLE DRAW REGION\nEdit this text, move it, undo and redo.')
        text.Name = 'P0A native editable text'
        if 'EDITABLE DRAW REGION' not in text.getString():
            raise RuntimeError('Native Draw text was not created')
        doc.getCurrentController().select(text)
        doc.setModified(False)  # New, private fixture only; never an existing user document.
        report = {'draw_pages': doc.getDrawPages().getCount(), 'preview_images': 2,
                  'preview_layer_locked': bool(layer.IsLocked),
                  'preview_layer_printable': bool(layer.IsPrintable),
                  'native_text_verified': 'EDITABLE DRAW REGION' in text.getString(),
                  'multi_page_layout_verified': False,
                  'renderer': 'external pdftoppm; synchronous setup only'}
        return doc, desktop, report
    except BaseException:
        doc.dispose()
        raise


def run(args):
    import uno
    with tempfile.TemporaryDirectory(prefix='p0a-preview-') as tmp:
        directory = Path(tmp)
        pdf = Path(args.pdf).resolve(strict=True) if args.pdf else directory / 'fixture.pdf'
        if not args.pdf:
            write_fixture(pdf)
        images = render(pdf, directory)
        pipe = 'p0a_preview_' + uuid.uuid4().hex
        command = [args.office, '-env:UserInstallation=' + (directory / 'profile').as_uri(),
                   '--nologo', '--nodefault', '--nofirststartwizard', '--norestore',
                   f'--accept=pipe,name={pipe};urp;StarOffice.ComponentContext']
        if args.headless:
            command.append('--headless')
        doc = desktop = None
        with (directory / 'office.log').open('w+') as log:
            process = subprocess.Popen(command, env=os.environ.copy(), stdout=log, stderr=subprocess.STDOUT)
            try:
                local = uno.getComponentContext()
                resolver = local.ServiceManager.createInstanceWithContext('com.sun.star.bridge.UnoUrlResolver', local)
                deadline = time.monotonic() + 30
                while True:
                    try:
                        remote = resolver.resolve(f'uno:pipe,name={pipe};urp;StarOffice.ComponentContext')
                        break
                    except Exception:
                        if process.poll() is not None or time.monotonic() > deadline:
                            raise RuntimeError('Cannot connect to private Office')
                        time.sleep(0.1)
                doc, desktop, report = build(remote, images)
                if args.save:
                    path = Path(args.save).resolve()
                    if path.exists():
                        raise FileExistsError(path)
                    doc.storeAsURL(path.as_uri(), (prop('FilterName', 'draw8'), prop('Overwrite', False)))
                print(json.dumps(report, indent=2), flush=True)
                if args.report:
                    with Path(args.report).open('x', encoding='utf-8') as output:
                        json.dump(report, output, indent=2)
                if not args.headless:
                    input('Check scroll/zoom, locked previews, editable text and undo. Enter closes the private Office: ')
            except BaseException:
                log.flush()
                log.seek(0)
                print(log.read())
                raise
            finally:
                try:
                    if doc is not None:
                        doc.dispose()
                    if desktop is not None:
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
    parser.add_argument('--pdf', help='Optional PDF with at least three pages')
    parser.add_argument('--save', help='New .odg fixture path, never PDF save')
    parser.add_argument('--report', help='New JSON report path')
    parser.add_argument('--headless', action='store_true', help='Structural checks only, no GUI verdict')
    run(parser.parse_args())
