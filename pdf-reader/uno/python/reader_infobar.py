# SPDX-License-Identifier: MPL-2.0
"""Draw-hosted InfoBar and frame dispatch wiring; no separate dialog window."""
import uno
import unohelper
from com.sun.star.frame import XDispatch, XDispatchProviderInterceptor, XInterceptorInfo
from com.sun.star.util import XCloseListener, CloseVetoException

BAR_ID = 'org.loongoffice.pdf.reader.mode'
EDIT = '.uno:EditDoc'
CLOSE = ('.uno:CloseDoc', '.uno:CloseWin')
SAVE = ('.uno:Save', '.uno:SaveAs', '.uno:SaveACopy', '.uno:ExportToPDF', '.uno:ExportDirectToPDF')


def command_url(command):
    url = uno.createUnoStruct('com.sun.star.util.URL')
    url.Complete = command
    url.Main = command
    url.Protocol, url.Path = '.uno:', command.removeprefix('.uno:')
    return url


class ReaderInfoBar(unohelper.Base, XDispatchProviderInterceptor, XInterceptorInfo,
                    XDispatch, XCloseListener):
    def __init__(self, document, commands):
        self.document, self.commands = document, commands
        self.controller = document.getCurrentController()
        self.frame = self.controller.getFrame()
        self.slave = self.master = None
        self.listeners = []
        self.last = None
        self.edit_mode = False
        self.edit_queued = False
        self.allow_close = False
        # Capture the native dispatch before interception. Invoke only from the
        # launcher's main loop, never recursively from a UNO UI callback.
        self.native_edit = self.frame.queryDispatch(command_url(EDIT), '', 0)
        if self.native_edit is None:
            raise RuntimeError('Native Edit Document dispatch is unavailable')
        self.frame.registerDispatchProviderInterceptor(self)
        try:
            document.addCloseListener(self)
            self.show('PDF 阅读模式', '点击“编辑”后，停留的页面会自动转换为可编辑对象。', edit=True)
        except BaseException:
            self.dispose()
            raise

    def show(self, title, message, *, edit=False, error=False):
        state = (title, message, edit, error)
        if state == self.last and self.controller.hasInfobar(BAR_ID):
            return
        buttons = []
        if edit:
            pair = uno.createUnoStruct('com.sun.star.beans.StringPair')
            pair.First, pair.Second = ('重试当前页' if self.edit_mode else '编辑'), EDIT
            buttons.append(pair)
        if self.last and edit == self.last[2] and self.controller.hasInfobar(BAR_ID):
            self.controller.updateInfobar(BAR_ID, title, message,
                uno.getConstantByName('com.sun.star.frame.InfobarType.WARNING' if error else
                                      'com.sun.star.frame.InfobarType.INFO'))
        else:
            if self.controller.hasInfobar(BAR_ID):
                self.controller.removeInfobar(BAR_ID)
            self.controller.appendInfobar(BAR_ID, title, message,
                uno.getConstantByName('com.sun.star.frame.InfobarType.WARNING' if error else
                                      'com.sun.star.frame.InfobarType.INFO'), tuple(buttons), False)
        if self.controller.hasInfobar('readonly'):
            self.controller.removeInfobar('readonly')
        self.last = state

    def set_edit_mode(self):
        self.edit_mode = True
        for listener, url in tuple(self.listeners):
            self._notify(listener, url)

    def getInterceptedURLs(self):
        return (EDIT,) + CLOSE + SAVE

    def queryDispatch(self, url, target, flags):
        if url.Complete in self.getInterceptedURLs():
            return self
        return self.slave.queryDispatch(url, target, flags) if self.slave else None

    def queryDispatches(self, descriptors):
        return tuple(self.queryDispatch(d.FeatureURL, d.FrameName, d.SearchFlags) for d in descriptors)

    def getSlaveDispatchProvider(self):
        return self.slave

    def setSlaveDispatchProvider(self, provider):
        self.slave = provider

    def getMasterDispatchProvider(self):
        return self.master

    def setMasterDispatchProvider(self, provider):
        self.master = provider

    def dispatch(self, url, arguments):
        if url.Complete == EDIT:
            if not self.edit_queued:
                self.edit_queued = True
                self.commands.put('edit')
        elif url.Complete in CLOSE:
            self.commands.put('close')
        elif url.Complete in SAVE:
            self.commands.put('save-unsupported')

    def _notify(self, listener, url):
        event = uno.createUnoStruct('com.sun.star.frame.FeatureStateEvent')
        event.Source, event.FeatureURL = self, url
        event.IsEnabled, event.State = url.Complete not in SAVE, self.edit_mode if url.Complete == EDIT else None
        listener.statusChanged(event)

    def addStatusListener(self, listener, url):
        self.listeners.append((listener, url))
        self._notify(listener, url)

    def removeStatusListener(self, listener, url):
        self.listeners = [(l, u) for l, u in self.listeners if l != listener or u.Complete != url.Complete]

    def queryClosing(self, event, ownership):
        if not self.allow_close and self.document.isModified():
            self.commands.put('close')
            raise CloseVetoException('PDF edit session needs discard confirmation', self)

    def notifyClosing(self, event):
        self.commands.put('closed')

    def disposing(self, event):
        self.commands.put('closed')

    def dispose(self):
        self.allow_close = True
        for cleanup in (lambda: self.controller.removeInfobar(BAR_ID) if self.controller.hasInfobar(BAR_ID) else None,
                        lambda: self.document.removeCloseListener(self),
                        lambda: self.frame.releaseDispatchProviderInterceptor(self)):
            try:
                cleanup()
            except Exception:
                pass  # A user may already have closed/disposed the frame.
        self.listeners.clear()
        self.slave = self.master = self.native_edit = None
