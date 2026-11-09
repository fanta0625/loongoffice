# SPDX-License-Identifier: MPL-2.0
"""Bounded ownership of disposable staged documents; never owns user edits."""
from collections import OrderedDict


class StagedPageCache:
    def __init__(self, max_pages=2, max_shapes=20000):
        self.max_pages, self.max_shapes = max_pages, max_shapes
        self.entries = OrderedDict()

    def __contains__(self, page):
        return page in self.entries

    def take(self, page):
        entry = self.entries.pop(page, None)
        return entry[0] if entry else None

    def retain(self, pages):
        for page in list(self.entries):
            if page not in pages:
                self.entries.pop(page)[0].dispose()

    def put(self, page, document, shapes):
        if shapes > self.max_shapes or self.max_pages < 1:
            document.dispose()
            return False
        old = self.entries.pop(page, None)
        if old:
            old[0].dispose()
        self.entries[page] = (document, shapes)
        while (len(self.entries) > self.max_pages
               or sum(entry[1] for entry in self.entries.values()) > self.max_shapes):
            self.entries.popitem(last=False)[1][0].dispose()
        return True

    def close(self):
        self.retain(set())
