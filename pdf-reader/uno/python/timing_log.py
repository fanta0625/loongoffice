# SPDX-License-Identifier: MPL-2.0
"""Monotonic diagnostic anchors; elapsed times are not GUI paint completion."""
from contextlib import contextmanager
import json
import time


class TimingLog:
    def __init__(self, enabled=False):
        self.enabled = enabled
        self.started = time.monotonic()
        self.events = []

    def mark(self, stage, **details):
        if self.enabled:
            event = dict(event='timing', stage=stage,
                         elapsed_ms=round((time.monotonic() - self.started) * 1000, 2),
                         **details)
            self.events.append(event)
            print(json.dumps(event, ensure_ascii=False), flush=True)

    @contextmanager
    def span(self, stage, **details):
        started = time.monotonic()
        self.mark(stage + '.begin', **details)
        try:
            yield
        except BaseException:
            self.mark(stage + '.failed', duration_ms=round((time.monotonic() - started) * 1000, 2), **details)
            raise
        else:
            self.mark(stage + '.end', duration_ms=round((time.monotonic() - started) * 1000, 2), **details)
