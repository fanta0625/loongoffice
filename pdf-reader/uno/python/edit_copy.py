# SPDX-License-Identifier: MPL-2.0
"""Diagnostics and short, exception-safe view-update batches for edit copies."""
from contextlib import contextmanager
import time


COPY_BATCH_SIZE = 16
COPY_BATCH_SECONDS = .10


class CopyProfile:
    def __init__(self, timing, page):
        self.timing, self.page = timing, page
        self.started = time.monotonic()
        self.last_progress = self.started
        self.seconds, self.calls = {}, {}
        self.batches = 0

    @contextmanager
    def phase(self, name):
        started = time.monotonic()
        try:
            yield
        finally:
            self.seconds[name] = self.seconds.get(name, 0) + time.monotonic() - started
            self.calls[name] = self.calls.get(name, 0) + 1

    def snapshot(self):
        return {'phase_ms': {k: round(v * 1000, 2) for k, v in self.seconds.items()},
                'phase_calls': dict(self.calls), 'batches': self.batches}

    def progress(self, completed, total):
        self.batches += 1
        now = time.monotonic()
        if now - self.last_progress >= 1 or completed == total:
            self.timing.mark('edit.copy.progress', page=self.page, completed=completed,
                             total=total, duration_ms=round((now - self.started) * 1000, 2),
                             **self.snapshot())
            self.last_progress = now


@contextmanager
def controller_batch(document, profile):
    # Draw's model lock is a boolean, not a nesting counter. Never release a
    # lock owned by another caller. An individual slow clone is not preemptible.
    with profile.phase('view_lock'):
        owned = not document.hasControllersLocked()
        if owned:
            document.lockControllers()
    try:
        yield
    finally:
        if owned:
            with profile.phase('view_unlock'):
                document.unlockControllers()
