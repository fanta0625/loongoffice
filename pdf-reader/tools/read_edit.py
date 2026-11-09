#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Open a real PDF in Draw and explicitly convert the selected page on request."""
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
from reader_session import ReaderSession
from edit_bridge import property_value as prop


def file_digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def panel(context, document, commands):
    import unohelper
    from com.sun.star.awt import XActionListener, XTopWindowListener

    class Listener(unohelper.Base, XActionListener, XTopWindowListener):
        def actionPerformed(self, event):
            commands.put(event.ActionCommand)  # No nested import in an AWT callback.

        def disposing(self, event):
            pass

        def windowClosing(self, event):
            commands.put('close')

        def windowOpened(self, event):
            pass

        def windowClosed(self, event):
            pass

        def windowMinimized(self, event):
            pass

        def windowNormalized(self, event):
            pass

        def windowActivated(self, event):
            pass

        def windowDeactivated(self, event):
            pass

    services = context.ServiceManager
    model = services.createInstanceWithContext('com.sun.star.awt.UnoControlDialogModel', context)
    model.Width, model.Height, model.Title = 260, 82, 'PDF 阅读与按页编辑'
    for name, service, label, x, y, width, height in (
        ('status', 'FixedText', '阅读模式：在 Draw 左侧选择页面', 8, 7, 244, 22),
        ('edit', 'Button', '编辑当前页', 8, 32, 112, 18),
        ('close', 'Button', '关闭此阅读会话', 132, 32, 120, 18),
        ('notice', 'FixedText', '开发版：尚不支持保存回 PDF；编辑转换不可撤销。', 8, 56, 244, 20),
    ):
        control = model.createInstance('com.sun.star.awt.UnoControl' + service + 'Model')
        control.PositionX, control.PositionY = x, y
        control.Width, control.Height, control.Label = width, height, label
        model.insertByName(name, control)
    dialog = services.createInstanceWithContext('com.sun.star.awt.UnoControlDialog', context)
    dialog.setModel(model)
    toolkit = services.createInstanceWithContext('com.sun.star.awt.Toolkit', context)
    parent = document.getCurrentController().getFrame().getContainerWindow()
    dialog.createPeer(toolkit, parent)
    listener = Listener()
    dialog.addTopWindowListener(listener)
    for name in ('edit', 'close'):
        button = dialog.getControl(name)
        button.setActionCommand(name)
        button.addActionListener(listener)
    dialog.setVisible(True)  # Modeless: Draw remains usable while panel is open.
    return dialog, listener, toolkit


def main(args):
    import uno
    source = Path(args.pdf).resolve(strict=True)
    if args.report and Path(args.report).exists():
        raise FileExistsError(args.report)
    digest = file_digest(source)
    with tempfile.TemporaryDirectory(prefix='pdf-read-edit-') as tmp:
        directory = Path(tmp)
        snapshot = directory / 'source.pdf'
        shutil.copyfile(source, snapshot)  # Stable source; never import from an edited/saved file.
        if file_digest(snapshot) != digest:
            raise RuntimeError('Source changed while taking the session snapshot')
        pipe = 'pdf_reader_' + uuid.uuid4().hex
        command = [args.office, '-env:UserInstallation=' + (directory / 'profile').as_uri(),
                   '--nologo', '--nodefault', '--nofirststartwizard', '--norestore',
                   f'--accept=pipe,name={pipe};urp;StarOffice.ComponentContext']
        env = os.environ.copy()
        env['LO_IMPORT_USE_PDFIUM'] = '1'
        env.pop('LO_PDF_HYBRID_VIEW_PROBE', None)  # Do not activate fixed three-page test hooks.
        session = desktop = dialog = None
        failure = None
        events = []
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
                            raise RuntimeError('Cannot connect to private Office')
                        time.sleep(.1)
                desktop = context.ServiceManager.createInstanceWithContext('com.sun.star.frame.Desktop', context)
                session = ReaderSession(context, snapshot.as_uri())
                session.document.setTitle(source.name + ' — PDF 阅读／按页编辑（开发版）')
                print(f'Reading {session.count} pages; editable imports so far: 0', flush=True)
                commands = SimpleQueue()
                dialog, listener, toolkit = panel(context, session.document, commands)
                last_status = None
                while process.poll() is None:
                    if commands.empty():
                        try:
                            page = session.current_page()
                            status = f'第 {page}/{session.count} 页：' + ('已导入，可直接编辑' if page in session.edited else '阅读预览')
                            if status != last_status:
                                dialog.getControl('status').setText(status)
                                dialog.getControl('edit').setEnable(page not in session.edited)
                                last_status = status
                        except uno.getClass('com.sun.star.lang.DisposedException'):
                            break
                        except RuntimeError as error:
                            dialog.getControl('status').setText(str(error))
                            dialog.getControl('edit').setEnable(False)
                            last_status = None
                        time.sleep(.1)
                        continue
                    action = commands.get()
                    if action == 'close':
                        if session.document.isModified():
                            dialog.setVisible(True)
                            box = toolkit.createMessageBox(dialog.getPeer(),
                                uno.Enum('com.sun.star.awt.MessageBoxType', 'QUERYBOX'),
                                uno.getConstantByName('com.sun.star.awt.MessageBoxButtons.BUTTONS_YES_NO'), '关闭阅读会话',
                                '修改尚未保存。本开发版不能保存回 PDF，确定放弃修改并关闭？')
                            answer = box.execute()
                            box.dispose()
                            if answer != uno.getConstantByName('com.sun.star.awt.MessageBoxResults.YES'):
                                continue
                        break
                    try:
                        dialog.getControl('edit').setEnable(False)
                        dialog.getControl('status').setText('正在转换当前页，请稍候…')
                        result = session.edit_current_page()
                        events.append(result)
                        print(json.dumps(result, ensure_ascii=False), flush=True)
                        last_status = None
                    except Exception as error:
                        print(f'Edit failed; reading page retained: {error}', flush=True)
                        box = toolkit.createMessageBox(dialog.getPeer(), uno.Enum('com.sun.star.awt.MessageBoxType', 'ERRORBOX'),
                            1, '页面编辑转换失败', str(error))
                        box.execute()
                        box.dispose()
                        last_status = None
            except BaseException as error:
                failure = {'type': type(error).__name__, 'message': str(error)}
                log.flush()
                log.seek(0)
                print(log.read(), file=sys.stderr)
                raise
            finally:
                report = {'source': str(source), 'source_sha256': digest, 'events': events,
                          'status': 'failed' if failure else 'closed', 'error': failure,
                          'edited_pages': sorted(session.edited) if session else [],
                          'import_calls': session.import_calls if session else [],
                          'renderer': 'Office internal PDFium graphics',
                          'tiles': False, 'continuous_layout': False, 'pdf_save_supported': False}
                for resource in (dialog, session):
                    if resource:
                        try:
                            resource.close() if resource is session else resource.dispose()
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
                if args.report:
                    with Path(args.report).open('x', encoding='utf-8') as output:
                        json.dump(report, output, indent=2, ensure_ascii=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--office', default='libreoffice')
    parser.add_argument('--pdf', required=True, help='Local non-encrypted PDF')
    parser.add_argument('--report', help='New JSON file written when the session closes')
    main(parser.parse_args())
