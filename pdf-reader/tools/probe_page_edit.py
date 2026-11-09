#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Selected-page converter and same-document session integration checks.

--adapter-only substitutes a one-page fixture importer to test the UNO session
against an unpatched Office. It never claims native Page= conversion passed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
from queue import SimpleQueue
import re
import subprocess
import sys
import tempfile
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'uno' / 'python'))
from edit_bridge import open_for_edit, property_value as prop
from reader_session import ReaderSession, set_graphic
from p0a_preview_probe import write_fixture
from probe_edit import collect_shapes, write_fixture as write_single


def run(args):
    import uno
    with tempfile.TemporaryDirectory(prefix='pdf-page-edit-check-') as tmp:
        directory = Path(tmp)
        pdf, single = directory / 'three.pdf', directory / 'one.pdf'
        write_fixture(pdf, page_count=args.pages)
        write_single(single)
        pipe = 'page_check_' + uuid.uuid4().hex
        command = [args.office, '-env:UserInstallation=' + (directory / 'profile').as_uri(),
                   '--nologo', '--nodefault', '--nofirststartwizard', '--norestore',
                   f'--accept=pipe,name={pipe};urp;StarOffice.ComponentContext']
        if not args.panel_smoke:
            command.append('--headless')
        env = os.environ.copy()
        env['LO_IMPORT_USE_PDFIUM'] = '1'
        env.pop('LO_PDF_HYBRID_VIEW_PROBE', None)
        session = desktop = keeper = None
        with (directory / 'office.log').open('w+') as log:
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
                        if process.poll() is not None or time.monotonic() > deadline:
                            raise RuntimeError('Cannot connect to test Office')
                        time.sleep(.1)
                desktop = context.ServiceManager.createInstanceWithContext('com.sun.star.frame.Desktop', context)
                # Keep the private application alive between staged documents,
                # including the rejected-import path that disposes its target.
                keeper = desktop.loadComponentFromURL('private:factory/sdraw', '_blank', 0,
                                                      (prop('Hidden', True),))
                if args.expect_unsupported:
                    try:
                        doc = open_for_edit(context, pdf.as_uri(), hidden=True, page=2)
                    except RuntimeError as error:
                        if 'Expected exactly one imported page' not in str(error):
                            raise
                        print('PASS: unpatched whole-document fallback was rejected')
                        return
                    doc.dispose()
                    raise RuntimeError('This Office supports Page=; negative test is inapplicable')
                if not args.adapter_only:
                    for number in range(1, args.pages + 1):
                        doc = open_for_edit(context, pdf.as_uri(), hidden=True, page=number)
                        try:
                            text = ' '.join(item['text'] for item in collect_shapes(doc.getDrawPages().getByIndex(0)))
                            normalized = ' '.join(text.split())
                            markers = re.findall(r'PDF SOURCE PAGE (\d+)\b', normalized)
                            if markers != [str(number)]:
                                raise RuntimeError(f'Wrong page text: {normalized!r}')
                        finally:
                            doc.dispose()
                    for number in (0, args.pages + 1):
                        try:
                            doc = open_for_edit(context, pdf.as_uri(), hidden=True, page=number)
                        except (RuntimeError, ValueError):
                            continue
                        doc.dispose()
                        raise RuntimeError('Out-of-range request accepted')

                failures = {3}
                calls = []

                def importer(ctx, url, *, hidden, page):
                    calls.append(page)
                    if page in failures:
                        raise RuntimeError('Injected conversion failure')
                    return open_for_edit(ctx, single.as_uri() if args.adapter_only else url,
                                         hidden=hidden, page=None if args.adapter_only else page)

                session = ReaderSession(context, pdf.as_uri(), hidden=not args.panel_smoke, importer=importer)
                doc = session.document
                if args.panel_smoke:
                    from read_edit import panel
                    commands = SimpleQueue()
                    dialog, listener, toolkit = panel(context, doc, commands)
                    try:
                        event = uno.createUnoStruct('com.sun.star.awt.ActionEvent')
                        event.ActionCommand = 'edit'
                        listener.actionPerformed(event)
                        if commands.get() != 'edit' or dialog.getControl('edit').getModel().Label != '编辑当前页':
                            raise RuntimeError('Panel action wiring failed')
                        listener.windowClosing(event)
                        if commands.get() != 'close':
                            raise RuntimeError('Panel close wiring failed')
                        box = toolkit.createMessageBox(dialog.getPeer(),
                            uno.Enum('com.sun.star.awt.MessageBoxType', 'QUERYBOX'),
                            uno.getConstantByName('com.sun.star.awt.MessageBoxButtons.BUTTONS_YES_NO'),
                            'Close test', 'Discard changes?')
                        box.dispose()  # Construct only; no unattended modal prompt.
                    finally:
                        dialog.dispose()
                if calls or session.count != args.pages:
                    raise RuntimeError('Editable conversion occurred during reading startup')
                for number in range(1, args.pages + 1):
                    preview = session._reading_preview(number, phase='probe startup')
                    if preview.Graphic is None or not preview.MoveProtect or not preview.SizeProtect:
                        raise RuntimeError('Owned preview lost its graphic or geometry protection')
                provider = context.ServiceManager.createInstanceWithContext(
                    'com.sun.star.graphic.GraphicProvider', context)
                preview_hashes = []
                for number in range(1, args.pages + 1):
                    image = directory / f'owned-preview-{number}.png'
                    provider.storeGraphic(session.previews[number - 1].Graphic, (
                        prop('URL', image.as_uri()), prop('MimeType', 'image/png')))
                    data = image.read_bytes()
                    if not data.startswith(b'\x89PNG\r\n\x1a\n') or len(data) < 24:
                        raise RuntimeError('Owned preview cannot render through internal graphics')
                    preview_hashes.append(hashlib.sha256(data).hexdigest())
                if len(set(preview_hashes)) != args.pages:
                    raise RuntimeError('Owned previews lost distinct source-page graphics')
                # A user's replacement graphic must never be mistaken for a
                # reading preview, even if it copies the name and layer.
                controller = doc.getCurrentController()
                untouched = session.pages.getByIndex(0)
                original = untouched.getByIndex(0)
                impostor = doc.createInstance('com.sun.star.drawing.GraphicObjectShape')
                untouched.add(impostor)
                set_graphic(impostor, original.Graphic)
                impostor.Name = original.Name
                impostor.LayerName = original.LayerName
                untouched.remove(original)
                controller.setCurrentPage(untouched)
                try:
                    session.edit_current_page()
                except RuntimeError as error:
                    if 'Reading preview integrity check failed on page 1' not in str(error):
                        raise
                else:
                    raise RuntimeError('Replacement graphic was accepted as a reading preview')
                finally:
                    untouched.remove(impostor)
                    untouched.add(original)
                if calls:
                    raise RuntimeError('Modified reading page reached the converter')
                doc.getUndoManager().clear()  # Discard test-only tampering setup.
                controller.setCurrentPage(session.pages.getByIndex(1))
                first = session.edit_current_page()
                if first['reused'] or calls != [2]:
                    raise RuntimeError('Wrong initial edit request')
                for number in set(range(1, args.pages + 1)) - {2}:
                    session._reading_preview(number, phase='after page 2 edit')
                page = session.pages.getByIndex(1)
                text_shape = next(page.getByIndex(i) for i in range(page.getCount())
                                  if hasattr(page.getByIndex(i), 'String') and page.getByIndex(i).String)
                text_shape.String += ' USER_EDIT_KEPT'
                history = tuple(doc.getUndoManager().getAllUndoActionTitles())
                controller.setCurrentPage(session.pages.getByIndex(2))
                try:
                    session.edit_current_page()
                except RuntimeError as error:
                    if str(error) != 'Injected conversion failure':
                        raise
                else:
                    raise RuntimeError('Injected failure was ignored')
                if session.pages.getByIndex(2).getCount() != 1 or session.edited != {2}:
                    raise RuntimeError('Failed conversion lost reading state')
                failures.clear()
                # Inject a failure after cloning starts. The original preview
                # and all prior-page objects/history must survive rollback.
                attach = session._attach_edit_layer
                def reject_clone(shape):
                    raise RuntimeError('Injected cloning failure')
                session._attach_edit_layer = reject_clone
                try:
                    session.edit_current_page()
                except RuntimeError as error:
                    if str(error) != 'Injected cloning failure':
                        raise
                else:
                    raise RuntimeError('Injected cloning failure was ignored')
                finally:
                    session._attach_edit_layer = attach
                if session.pages.getByIndex(2).getCount() != 1 or session.edited != {2}:
                    raise RuntimeError('Clone rollback lost the original preview')
                session.edit_current_page()
                controller.setCurrentPage(page)
                if not session.edit_current_page()['reused'] or calls != [2, 3, 3, 3]:
                    raise RuntimeError('Edited page was imported twice')
                if 'USER_EDIT_KEPT' not in text_shape.String:
                    raise RuntimeError('Switching pages lost edited text')
                if tuple(doc.getUndoManager().getAllUndoActionTitles()) != history:
                    raise RuntimeError('Page conversion changed existing edit undo history')
                if session.pages.getByIndex(0).getCount() != 1 or session.count != args.pages:
                    raise RuntimeError('Untouched reading page changed')
                print(json.dumps({'status': 'passed', 'native_page_selection_verified': not args.adapter_only,
                                  'source_pages': session.count,
                                  'same_document': True, 'failure_retains_preview': True,
                                  'panel_smoke': args.panel_smoke,
                                  'clone_failure_rollback': True, 'previous_undo_history_retained': True,
                                  'owned_previews_verified': True, 'replacement_graphic_rejected': True,
                                  'distinct_preview_rendering_verified': True,
                                  'repeat_edit_reuses_objects': True, 'edited_text_retained': True,
                                  'calls': calls}, indent=2))
            except BaseException:
                log.flush()
                log.seek(0)
                print(log.read(), file=sys.stderr)
                raise
            finally:
                if session:
                    try:
                        session.close()
                    except Exception:
                        pass
                if desktop:
                    if keeper:
                        try:
                            keeper.dispose()
                        except Exception:
                            pass
                    try:
                        desktop.terminate()
                    except Exception:
                        pass
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--office', default='libreoffice')
    parser.add_argument('--pages', type=int, default=10, help='Generated PDF page count (at least 3)')
    parser.add_argument('--panel-smoke', action='store_true', help='Also create the modeless controls on a GUI desktop')
    group = parser.add_mutually_exclusive_group()
    group.add_argument('--adapter-only', action='store_true')
    group.add_argument('--expect-unsupported', action='store_true')
    args = parser.parse_args()
    if args.pages < 3:
        parser.error('--pages must be at least 3')
    run(args)
