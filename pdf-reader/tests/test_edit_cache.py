# SPDX-License-Identifier: MPL-2.0
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'uno' / 'python'))
from edit_cache import StagedPageCache


class CacheTests(unittest.TestCase):
    def test_budget_disposes_old_stages_but_take_transfers_ownership(self):
        cache = StagedPageCache(max_pages=2, max_shapes=10)
        a, b, c = Mock(), Mock(), Mock()
        cache.put(1, a, 4)
        cache.put(2, b, 4)
        cache.put(3, c, 4)
        a.dispose.assert_called_once()
        self.assertIs(cache.take(2), b)
        cache.close()
        b.dispose.assert_not_called()
        c.dispose.assert_called_once()

    def test_distant_pages_and_oversized_stages_are_disposed(self):
        cache = StagedPageCache(max_shapes=10)
        a, b = Mock(), Mock()
        cache.put(1, a, 2)
        cache.retain({9, 11})
        a.dispose.assert_called_once()
        self.assertFalse(cache.put(9, b, 11))
        b.dispose.assert_called_once()
        self.assertNotIn(9, cache)
