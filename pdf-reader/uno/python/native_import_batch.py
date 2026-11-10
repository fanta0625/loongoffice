# SPDX-License-Identifier: MPL-2.0
"""Use Draw's existing XModifiable2 import-update suppression.

Controller locking defers SdrObject broadcasts, but DrawDocShell::SetModified
has a separate DocChanged broadcast, even when the document is already dirty.
Keep that channel quiet during construction and rollback. The caller publishes
the final modified state once, after this guard restores the original setting.
"""
from contextlib import contextmanager


@contextmanager
def modification_batch(document, profile):
    # Use the explicit query: the IDL description of disableSetModified's
    # return value differs from SfxBaseModel's implementation in this baseline.
    with profile.phase('modification_suspend'):
        enabled = document.isSetModifiedEnabled()
        if enabled:
            document.disableSetModified()
    try:
        yield
    finally:
        if enabled:
            with profile.phase('modification_restore'):
                document.enableSetModified()
