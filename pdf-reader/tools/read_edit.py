#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Read a PDF in Draw; enter edit mode once and convert settled pages on demand."""
import argparse
import hashlib
import json
import os
from pathlib import Path
from queue import SimpleQueue
import shutil
import subprocess
import sys
import tempfile
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'uno' / 'python'))
from reader_session import ReaderSession, SessionClosed
from reader_infobar import ReaderInfoBar
from edit_mode import EditMode
from viewport_tiles import ViewportTiles
from timing_log import TimingLog


def file_digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main(args):
    timing = TimingLog(enabled=True, wall_clock=args.trace_gestures)
    timing.mark('startup.begin')
    with timing.span('uno.import'):
        import uno
    source = Path(args.pdf).resolve(strict=True)
    if args.report and Path(args.report).exists():
        raise FileExistsError(args.report)
    with timing.span('source.hash', bytes=source.stat().st_size):
        digest = file_digest(source)
    with tempfile.TemporaryDirectory(prefix='pdf-read-edit-') as tmp:
        directory = Path(tmp)
        snapshot = directory / 'source.pdf'
        with timing.span('source.snapshot'):
            shutil.copyfile(source, snapshot)  # Stable source; never import from an edited/saved file.
        with timing.span('source.verify'):
            if file_digest(snapshot) != digest:
                raise RuntimeError('Source changed while taking the session snapshot')
        pipe = 'pdf_reader_' + uuid.uuid4().hex
        command = [args.office, '-env:UserInstallation=' + (directory / 'profile').as_uri(),
                   '--nologo', '--nodefault', '--nofirststartwizard', '--norestore',
                   f'--accept=pipe,name={pipe};urp;StarOffice.ComponentContext']
        env = os.environ.copy()
        env['LO_IMPORT_USE_PDFIUM'] = '1'
        if args.trace_gestures:
            env['LO_PDF_EDIT_GESTURE_TRACE'] = '1'
        env.pop('LO_PDF_HYBRID_VIEW_PROBE', None)  # Do not activate fixed three-page test hooks.
        session = desktop = ui = engine = tiles = None
        failure = None
        exit_reason = 'office-process-exit'
        events = []
        log_path = Path(args.trace_gestures) if args.trace_gestures else directory / 'office.log'
        with log_path.open('x+', encoding='utf-8', errors='replace') as log:
            timing.mark('office.launch.begin')
            process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
            timing.mark('office.launch.end', pid=process.pid)
            try:
                timing.mark('uno.connect.begin')
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
                timing.mark('uno.connect.end')
                desktop = context.ServiceManager.createInstanceWithContext('com.sun.star.frame.Desktop', context)
                with timing.span('reader.session'):
                    session = ReaderSession(context, snapshot.as_uri(), read_only=True, timing=timing,
                                            direct_edit=not args.legacy_edit_copy,
                                            guard_automatic_edit=False)
                session.document.setTitle(source.name + ' — PDF 阅读／按页编辑（开发版）')
                print(f'Reading {session.count} pages; editable imports so far: 0', flush=True)
                commands = SimpleQueue()
                with timing.span('infobar.setup'):
                    ui = ReaderInfoBar(session.document, commands)
                engine = EditMode(session, lambda: session.enable_editing(ui.native_edit),
                                  prefetch_seconds=None if args.no_prefetch or not args.legacy_edit_copy else 2)
                # Keep settled-page auto conversion, but let Draw own mouse,
                # drag and tool state. Native gesture guards are not enabled.
                timing.mark('edit.policy', automatic_pages=True,
                            settle_seconds=engine.settle_seconds,
                            mouse_guard=False, tool_control='native-draw')
                if not args.no_tiles:
                    with timing.span('tiles.setup'):
                        tiles = ViewportTiles(session, directory / 'tiles', timing=timing)
                toolkit = context.ServiceManager.createInstanceWithContext('com.sun.star.awt.Toolkit', context)
                transition_error = None
                timing.mark('reader.loop.ready')  # Scheduler ready, not first GUI paint.
                while process.poll() is None:
                    try:
                        if not commands.empty():
                            action = commands.get()
                            if action == 'closed':
                                exit_reason = 'document-close-notification'
                                break
                            if action == 'save-unsupported':
                                transition_error = '本开发版尚不支持保存完整 PDF；原文件不会被覆盖。'
                            if action == 'close':
                                if session.document.isModified():
                                    box = toolkit.createMessageBox(ui.frame.getContainerWindow(),
                                        uno.Enum('com.sun.star.awt.MessageBoxType', 'QUERYBOX'),
                                        uno.getConstantByName('com.sun.star.awt.MessageBoxButtons.BUTTONS_YES_NO'),
                                        '关闭 PDF', '修改尚未保存。本开发版不能保存回 PDF，确定放弃修改并关闭？')
                                    try:
                                        if box.execute() != uno.getConstantByName('com.sun.star.awt.MessageBoxResults.YES'):
                                            continue
                                    finally:
                                        box.dispose()
                                ui.allow_close = True
                                exit_reason = 'requested-close'
                                break
                            if action == 'edit':
                                try:
                                    ui.show('正在进入编辑模式', '请稍候…')
                                    if tiles:
                                        tiles.clear()  # Never feed preview overlays to the editable importer.
                                    engine.request_edit()
                                    ui.set_edit_mode()
                                    transition_error = None
                                    event = {'event': 'edit-mode', 'view': session.view_state()}
                                    events.append(event)
                                    print(json.dumps(event, ensure_ascii=False), flush=True)
                                except Exception as error:
                                    transition_error = str(error)
                                    event = {'event': 'edit-mode-failed', 'error': transition_error}
                                    events.append(event)
                                    print(json.dumps(event, ensure_ascii=False), flush=True)
                                finally:
                                    ui.edit_queued = False
                        result = engine.tick(time.monotonic())
                        if result:
                            events.append(result)
                            print(json.dumps(result, ensure_ascii=False), flush=True)
                        if tiles and engine.mode == 'reading':
                            tile_event = tiles.tick(time.monotonic())
                            if tile_event:
                                events.append(tile_event)
                                print(json.dumps(tile_event, ensure_ascii=False), flush=True)
                        page = session.current_page()
                        if transition_error:
                            ui.show('PDF 会话提示', transition_error,
                                    edit=engine.mode == 'reading', error=True)
                        elif engine.mode == 'reading':
                            hint = ('高清瓦片不可用，保留整页预览。' if tiles and tiles.error else
                                    '缩放或滚动停留后更新可见区域高清瓦片。' if tiles else '')
                            ui.show('PDF 阅读模式', f'第 {page}/{session.count} 页；{hint}点击“编辑”进入编辑模式。', edit=True)
                        elif page in engine.failures:
                            ui.show('当前页转换失败，预览已保留', engine.failures[page], edit=True, error=True)
                        else:
                            state = '可直接编辑' if page in session.edited else '页面停留后自动准备编辑'
                            ui.show('PDF 编辑模式', f'第 {page}/{session.count} 页：{state}。开发版暂不支持保存回 PDF。')
                    except (SessionClosed, uno.getClass('com.sun.star.lang.DisposedException')) as error:
                        exit_reason = 'session-unavailable'
                        event = {'event': 'session-unavailable', 'type': type(error).__name__,
                                 'message': str(error)}
                        events.append(event)
                        print(json.dumps(event, ensure_ascii=False), flush=True)
                        break
                    except RuntimeError as error:
                        ui.show('页面状态异常', str(error), error=True)
                    time.sleep(.1)
            except BaseException as error:
                exit_reason = 'python-exception'
                failure = {'type': type(error).__name__, 'message': str(error)}
                log.flush()
                log.seek(0)
                print(log.read(), file=sys.stderr)
                raise
            finally:
                # Capture before our own terminate/kill cleanup changes the exit code.
                office_exit = {'reason': exit_reason, 'returncode_before_cleanup': process.poll(),
                               'log': str(log_path) if args.trace_gestures else None}
                if office_exit['returncode_before_cleanup'] not in (None, 0) and failure is None:
                    failure = {'type': 'OfficeProcessExit',
                               'message': 'Office exited with code '
                                          + str(office_exit['returncode_before_cleanup'])}
                print(json.dumps({'event': 'reader.exit', **office_exit}, ensure_ascii=False), flush=True)
                report = {'source': str(source), 'source_sha256': digest, 'events': events,
                          'office_exit': office_exit,
                          'timings': timing.events,
                          'status': 'failed' if failure else 'closed', 'error': failure,
                          'edited_pages': sorted(session.edited) if session else [],
                          'import_calls': session.import_calls if session else [],
                          'renderer': 'Office internal PDFium graphics',
                          'mode': engine.mode if engine else 'reading', 'ui': 'Draw InfoBar',
                          'page_failures': engine.failures if engine else {},
                          'tiles': bool(tiles and tiles.metrics['published']),
                          'tile_reading': dict(tiles.metrics, error=tiles.error) if tiles else None,
                          'continuous_layout': False, 'pdf_save_supported': False}
                report['edit_route'] = 'legacy-copy' if args.legacy_edit_copy else 'native-direct-page'
                report['prefetch'] = {'enabled': not args.no_prefetch and args.legacy_edit_copy, 'neighbours_each_side': 1,
                                      'max_staged_pages': 2, 'max_staged_shapes': 20000,
                                      'synchronous_import_cancellable': False}
                for resource in (tiles, ui, session):
                    if resource:
                        try:
                            resource.dispose() if resource is ui else resource.close()
                        except Exception:
                            pass
                if desktop:
                    try:
                        desktop.terminate()
                    except Exception:
                        pass
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                office_exit['returncode_after_cleanup'] = process.returncode
                if args.report:
                    with Path(args.report).open('x', encoding='utf-8') as output:
                        json.dump(report, output, indent=2, ensure_ascii=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--office', default='libreoffice')
    parser.add_argument('--pdf', required=True, help='Local non-encrypted PDF')
    parser.add_argument('--report', help='New JSON file written when the session closes')
    parser.add_argument('--trace-gestures', metavar='OFFICE_LOG',
                        help='Record native thumbnail gestures in a new log file')
    parser.add_argument('--no-prefetch', action='store_true',
                        help='Disable synchronous idle neighbour preparation (useful for very slow pages)')
    parser.add_argument('--no-tiles', action='store_true', help='Use only the original whole-page preview')
    parser.add_argument('--legacy-edit-copy', action='store_true',
                        help='Use the old temporary-document copier for comparison only')
    main(parser.parse_args())
