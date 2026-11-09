# SPDX-License-Identifier: MPL-2.0
"""Document mode and settled-page conversion, independent of UNO widgets.

Only the active Draw page is observed in this single-page host. This is not a
continuous viewport scheduler, a tile cache, or cancellable background import.
"""


class EditMode:
    def __init__(self, session, enable_editing, *, settle_seconds=.35):
        self.session = session
        self.enable_editing = enable_editing
        self.settle_seconds = settle_seconds
        self.mode = 'reading'
        self.candidate = None
        self.since = 0
        self.failures = {}

    def request_edit(self):
        if self.mode == 'reading':
            self.enable_editing()
            self.mode = 'editing'
        self.failures.pop(self.session.current_page(), None)

    def tick(self, now):
        page = self.session.current_page()
        if self.mode == 'reading':
            return None
        if page != self.candidate:
            self.candidate, self.since = page, now
        if (page in self.session.edited or page in self.failures
                or now - self.since < self.settle_seconds):
            return None
        try:
            return self.session.edit_current_page()
        except Exception as error:
            # Keep the preview; never retry every polling tick or destroy an
            # already edited page. The InfoBar offers an explicit retry.
            self.failures[page] = str(error)
            return {'page': page, 'error': str(error), 'preview_retained': True}
