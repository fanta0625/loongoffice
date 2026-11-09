# SPDX-License-Identifier: MPL-2.0
"""Document mode and settled-page conversion, independent of UNO widgets.

The active page has priority. Adjacent pages are staged synchronously after
idle time; this is not a tile cache or an interruptible background importer.
"""
import time


class EditMode:
    def __init__(self, session, enable_editing, *, settle_seconds=.35, prefetch_seconds=2):
        self.session = session
        self.enable_editing = enable_editing
        self.settle_seconds = settle_seconds
        self.mode = 'reading'
        self.candidate = None
        self.since = 0
        self.failures = {}
        self.prefetch_seconds = prefetch_seconds
        self.prefetch_attempted = set()

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
            self.prefetch_attempted.clear()
        if page in self.failures or now - self.since < self.settle_seconds:
            return None
        if page in self.session.edited:
            if self.prefetch_seconds is None or now - self.since < self.prefetch_seconds:
                return None
            for target in (page + 1, page - 1):
                if (1 <= target <= self.session.count and target not in self.session.edited
                        and target not in self.prefetch_attempted):
                    self.prefetch_attempted.add(target)
                    try:
                        return self.session.prefetch_page(target, page)
                    except Exception as error:
                        # Speculation never blocks foreground conversion or
                        # puts a neighbouring page into the explicit-retry set.
                        return {'event': 'prefetch-failed', 'page': target, 'error': str(error)}
            return None
        try:
            started = time.monotonic()
            result = self.session.edit_current_page()
            # Start the idle window AFTER the potentially long conversion.
            # Caller clocks may be synthetic; preserve their time base.
            self.since = now + time.monotonic() - started
            return result
        except Exception as error:
            # Keep the preview; never retry every polling tick or destroy an
            # already edited page. The InfoBar offers an explicit retry.
            self.failures[page] = str(error)
            return {'page': page, 'error': str(error), 'preview_retained': True}
