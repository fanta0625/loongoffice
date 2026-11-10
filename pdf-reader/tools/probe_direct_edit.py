#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Verify native target-page import against a rebuilt private Office instance."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from unittest.mock import patch
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'uno' / 'python'))
from direct_edit import import_into_page, DIRECT_IMPORTER
from edit_bridge import open_for_edit
from reader_session import ReaderSession
from p0a_preview_probe import write_fixture
from probe_page_edit import geometry_tree, render_page


def trees(page):
    return [geometry_tree(page.getByIndex(i)) for i in range(page.getCount())]


def matching_geometry(expected, actual):
    if len(expected) != len(actual):
        return False
    for left, right in zip(expected, actual):
        if (left[0] != right[0] or any(abs(a - b) > 1 for a, b in zip(left[1], right[1]))
                or not matching_geometry(left[2], right[2])):
            return False
    return True


def geometry_differences(expected, actual, path='page', limit=10):
    """Keep strict comparison, but report where structure or geometry differs."""
    differences = []
    if len(expected) != len(actual):
        differences.append({'path': path, 'reason': 'object-count',
                            'expected': len(expected), 'actual': len(actual)})
    for index, (left, right) in enumerate(zip(expected, actual)):
        location = f'{path}/{index}'
        if left[0] != right[0] or left[1] != right[1]:
            delta = [b - a for a, b in zip(left[1], right[1])]
            if left[0] != right[0] or any(abs(value) > 1 for value in delta):
                differences.append({'path': location, 'expected_type': left[0],
                                    'actual_type': right[0], 'expected_geometry': left[1],
                                    'actual_geometry': right[1], 'delta': delta})
        differences.extend(geometry_differences(left[2], right[2], location, limit))
        if len(differences) >= limit:
            break
    return differences[:limit]


def layer_tree(page, name):
    for i in range(page.getCount()):
        shape = page.getByIndex(i)
        if shape.LayerName != name:
            raise RuntimeError('An imported shape escaped the edit layer')
        if shape.supportsService('com.sun.star.drawing.GroupShape'):
            layer_tree(shape, name)


def seed_native_undo(context, document, page):
    """Use Draw's delete command to create a genuine document undo entry.

    XText.setString uses QuickInsertText/UpdateData; it is not a UI typing
    operation and need not create a document undo entry.
    """
    undo = document.getUndoManager()
    if undo.isLocked():
        raise RuntimeError('Direct import left the undo manager locked')
    marker = document.createInstance('com.sun.star.drawing.RectangleShape')
    marker.Name = 'PDF_NATIVE_UNDO_PROBE'
    page.add(marker)
    controller = document.getCurrentController()
    if not controller.select(marker):
        raise RuntimeError('Cannot select the native undo fixture')
    count = page.getCount()
    dispatch = context.ServiceManager.createInstanceWithContext(
        'com.sun.star.frame.DispatchHelper', context)
    dispatch.executeDispatch(controller.getFrame(), '.uno:Delete', '', 0, ())
    deadline = time.monotonic() + 3
    while page.getCount() == count and time.monotonic() < deadline:
        time.sleep(.05)
    if page.getCount() != count - 1 or not undo.isUndoPossible():
        raise RuntimeError('Native Draw Delete did not remove the fixture with an undo entry')
    return marker


def text_properties(page):
    """Inspect only the small test fixture, never the production edit path."""
    names = ('CharFontName', 'CharFontNameAsian', 'CharFontNameComplex',
             'CharHeight', 'CharWeight', 'CharPosture', 'CharScaleWidth',
             'CharAutoKerning', 'CharKerning', 'ParaAdjust', 'ParaLeftMargin',
             'ParaRightMargin', 'TextAutoGrowWidth', 'TextAutoGrowHeight',
             'TextFitToSize', 'TextLeftDistance', 'TextRightDistance')
    def properties(obj):
        values = {}
        info = obj.getPropertySetInfo()
        for name in names:
            if info.hasPropertyByName(name):
                value = obj.getPropertyValue(name)
                values[name] = value if value is None or isinstance(value, (str, int, float, bool)) else str(value)
        return values
    result = []
    for index in range(page.getCount()):
        shape = page.getByIndex(index)
        if not hasattr(shape, 'getString'):
            continue
        entry = {'index': index, 'text': shape.getString(), 'shape': properties(shape), 'runs': []}
        paragraphs = shape.createEnumeration()
        while paragraphs.hasMoreElements():
            paragraph = paragraphs.nextElement()
            portions = paragraph.createEnumeration()
            while portions.hasMoreElements():
                portion = portions.nextElement()
                entry['runs'].append({'text': portion.getString(), 'properties': properties(portion)})
        result.append(entry)
    return result


def run(args):
    import uno
    with tempfile.TemporaryDirectory(prefix='pdf-direct-check-') as tmp:
        root = Path(tmp)
        pdf = root / 'fixture.pdf'
        write_fixture(pdf, page_count=3)
        pipe = 'direct_check_' + uuid.uuid4().hex
        command = [args.office, '-env:UserInstallation=' + (root / 'profile').as_uri(),
                   '--headless', '--nologo', '--nodefault', '--nofirststartwizard', '--norestore',
                   f'--accept=pipe,name={pipe};urp;StarOffice.ComponentContext']
        env = dict(os.environ, LO_IMPORT_USE_PDFIUM='1')
        session = desktop = None
        with (root / 'office.log').open('w+') as log:
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
                            raise RuntimeError('Cannot connect to private Office')
                        time.sleep(.1)
                if DIRECT_IMPORTER not in context.ServiceManager.getAvailableServiceNames():
                    raise RuntimeError('Native direct-page service unavailable; rebuild Office first')
                desktop = context.ServiceManager.createInstanceWithContext('com.sun.star.frame.Desktop', context)
                session = ReaderSession(context, pdf.as_uri(), hidden=True, direct_edit=True)
                controller = session.document.getCurrentController()
                pages = session.pages
                original_pages = [pages.getByIndex(i) for i in range(3)]
                names = [page.Name for page in original_pages]
                dimensions = [(page.Width, page.Height) for page in original_pages]
                controller.setCurrentPage(original_pages[1])
                # Independent existing import is a test oracle only. Production
                # direct import never creates this temporary Draw model.
                oracle = open_for_edit(context, pdf.as_uri(), hidden=True, page=2)
                try:
                    oracle_page = oracle.getDrawPages().getByIndex(0)
                    expected = trees(oracle_page)
                    expected_size = [oracle_page.Width, oracle_page.Height]
                    expected_text = text_properties(oracle_page)
                finally:
                    oracle.dispose()
                def checked_import(*values):
                    if session.document.isSetModifiedEnabled():
                        raise RuntimeError('Native modified-state updates were not suppressed')
                    return import_into_page(*values)
                with patch('reader_session.import_into_page', side_effect=checked_import):
                    result = session.edit_current_page()
                if not session.document.isSetModifiedEnabled() or not session.document.isModified():
                    raise RuntimeError('Native modified-state updates were not restored/published')
                if result['copied_shapes'] != 0 or result['temporary_draw_documents'] != 0:
                    raise RuntimeError('Direct path used the old copier')
                actual = trees(original_pages[1])
                if not matching_geometry(expected, actual):
                    diagnostics = {'status': 'failed', 'stage': 'geometry-comparison',
                                   'expected_page_size': expected_size,
                                   'actual_page_size': list(dimensions[1]),
                                   'geometry_units': '1/100 mm; x,y,width,height',
                                   'differences': geometry_differences(expected, actual),
                                   'expected': expected, 'actual': actual,
                                   'expected_text': expected_text,
                                   'actual_text': text_properties(original_pages[1]),
                                   'first_result': result}
                    print(json.dumps(diagnostics, indent=2), flush=True)
                    if args.report:
                        Path(args.report).write_text(json.dumps(diagnostics, indent=2), encoding='utf-8')
                    raise RuntimeError('Direct object geometry differs from existing importer')
                layer_tree(original_pages[1], session.edit_layer.Name)
                text = None
                for i in range(original_pages[1].getCount()):
                    candidate = original_pages[1].getByIndex(i)
                    if hasattr(candidate, 'getString') and 'PDF SOURCE PAGE 2' in candidate.getString():
                        text = candidate
                if text is None:
                    raise RuntimeError('Native selected page did not produce expected editable text')
                text.setString('DIRECT_USER_EDIT')
                undo = session.document.getUndoManager()
                marker = seed_native_undo(context, session.document, original_pages[1])
                history = tuple(undo.getAllUndoActionTitles())
                if not history:
                    raise RuntimeError('Native Delete did not create undo history')
                before = render_page(context, original_pages[1], root / 'before.png')
                controller.setCurrentPage(original_pages[2])
                session.edit_current_page()
                if history != tuple(undo.getAllUndoActionTitles()):
                    raise RuntimeError('Direct import changed prior undo history')
                if before != render_page(context, original_pages[1], root / 'after.png'):
                    raise RuntimeError('Importing another page changed an already edited page')
                controller.setCurrentPage(original_pages[1])
                undo.undo()
                if not any(original_pages[1].getByIndex(i) == marker
                           for i in range(original_pages[1].getCount())):
                    raise RuntimeError('Retained native undo did not restore the deleted fixture')
                undo.redo()
                if any(original_pages[1].getByIndex(i) == marker
                       for i in range(original_pages[1].getCount())):
                    raise RuntimeError('Retained native redo did not delete the fixture')
                if text.getString() != 'DIRECT_USER_EDIT':
                    raise RuntimeError('Undo/redo damaged the edited PDF text')
                controller.setCurrentPage(original_pages[0])
                preview = session._reading_preview(1)
                def fail_after_native(*values):
                    import_into_page(*values)
                    raise RuntimeError('injected post-import failure')
                with patch('reader_session.import_into_page', side_effect=fail_after_native):
                    try:
                        session.edit_current_page()
                    except RuntimeError as error:
                        if str(error) != 'injected post-import failure':
                            raise
                    else:
                        raise RuntimeError('Expected injected failure')
                if original_pages[0].getCount() != 1 or original_pages[0].getByIndex(0) != preview:
                    raise RuntimeError('Failed import did not retain the preview')
                if not session.document.isSetModifiedEnabled():
                    raise RuntimeError('Failed import leaked native modification suppression')
                def navigate_after_native(*values):
                    import_into_page(*values)
                    controller.setCurrentPage(original_pages[1])
                with patch('reader_session.import_into_page', side_effect=navigate_after_native):
                    cancelled = session.edit_current_page()
                if not cancelled.get('cancelled') or original_pages[0].getCount() != 1:
                    raise RuntimeError('Navigation before commit did not roll back')
                if pages.getCount() != 3 or any(pages.getByIndex(i) != p for i, p in enumerate(original_pages)):
                    raise RuntimeError('Reader page identities changed')
                if names != [p.Name for p in original_pages] or dimensions != [(p.Width, p.Height) for p in original_pages]:
                    raise RuntimeError('Reader page names/dimensions changed')
                print(json.dumps({'status': 'passed', 'edit_route': 'native-direct-page',
                                  'copied_shapes': 0, 'geometry_matches_existing_importer': True,
                                  'native_entry': 'PDFPageImportFilter',
                                  'native_layer_assignment': True, 'existing_page_render_retained': True,
                                  'previous_undo_history_retained': True,
                                  'native_delete_undo_redo_verified': True,
                                  'native_modification_batch_verified': True,
                                  'ui_text_typing_undo_verified': False,
                                  'failure_retains_preview': True, 'navigation_before_commit_rollback': True,
                                  'page_identity_retained': True, 'first_result': result}, indent=2))
            finally:
                if session:
                    session.close()
                if desktop:
                    desktop.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--office', default='libreoffice')
    parser.add_argument('--report', help='Write geometry failure diagnostics as JSON')
    run(parser.parse_args())
