# SPDX-License-Identifier: MPL-2.0
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'uno' / 'python'))
from viewport_tiles import plan_tiles, TileCache, ViewportTiles
from tile_bridge import validate_tile, TileBridgeUnavailable


class ViewportTests(unittest.TestCase):
    def test_zoom_requests_more_source_pixels_for_same_physical_region(self):
        bounds = (0, 0, 21000, 29700)
        a = plan_tiles(1, bounds, (0, 0, 10000, 10000), 100)
        b = plan_tiles(1, bounds, (0, 0, 5000, 5000), 200)
        self.assertGreaterEqual(b.pixels[0], a.pixels[0] * 2 - 1)
        self.assertLessEqual(len(b.rects), 64)
        for rect in b.rects:
            validate_tile(b.pixels, rect)

    def test_view_offset_clips_to_pdf_and_keeps_partial_edge_tiles(self):
        bounds = (123, 456, 21000, 29700)
        plan = plan_tiles(2, bounds, (20000, 29000, 5000, 5000), 400)
        self.assertTrue(plan.rects)
        self.assertTrue(any(x + w == plan.pixels[0] for x, y, w, h in plan.rects))
        self.assertTrue(any(y + h == plan.pixels[1] for x, y, w, h in plan.rects))
        self.assertFalse(plan_tiles(2, bounds, (-10000, -10000, 100, 100), 100).rects)

    def test_extreme_zoom_remains_bounded_and_prioritizes_viewport_centre(self):
        plan = plan_tiles(1, (0, 0, 21000, 29700), (0, 0, 21000, 29700), 3000)
        self.assertEqual(len(plan.rects), 64)
        self.assertLessEqual(max(plan.pixels), 131072)
        x, y, w, h = plan.rects[0]
        self.assertLess(abs(x + w / 2 - plan.pixels[0] / 2), 512)
        self.assertLess(abs(y + h / 2 - plan.pixels[1] / 2), 512)

    def test_lru_evicts_files_by_decoded_cost_and_reuses_recent_tile(self):
        with tempfile.TemporaryDirectory() as tmp:
            cache = TileCache(tmp, budget=8)
            for key in ('a', 'b'):
                path = Path(tmp) / key
                path.write_bytes(b'png')
                cache.put(key, path, 4)
            cache.get('a')
            path = Path(tmp) / 'c'
            path.write_bytes(b'png')
            cache.put('c', path, 4)
            self.assertFalse((Path(tmp) / 'b').exists())
            self.assertIsNotNone(cache.get('a'))
            self.assertEqual(cache.bytes, 8)
            cache.close()
            self.assertFalse(list(Path(tmp).iterdir()))


class LoopTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        session = SimpleNamespace(previews=[Mock(), Mock()])
        self.renderer = Mock()
        self.renderer.render.side_effect = lambda graphic, path, *args: path.write_bytes(b'png')
        self.tiles = ViewportTiles(session, self.tmp.name, renderer=self.renderer)
        self.first = plan_tiles(1, (0, 0, 10000, 10000), (0, 0, 10000, 10000), 100)
        self.second = plan_tiles(2, self.first.bounds, (0, 0, 10000, 10000), 100)
        self.current = self.first
        self.tiles.snapshot = lambda: self.current
        self.tiles.remove_except = Mock()
        self.tiles.install = Mock()

    def tearDown(self):
        self.tiles.cache.close()
        self.tmp.cleanup()

    def test_rapid_navigation_does_not_render_old_page_queue(self):
        self.tiles.tick(0)
        self.current = self.second
        self.tiles.tick(.1)
        self.tiles.tick(.2)
        self.renderer.render.assert_not_called()
        event = self.tiles.tick(.4)
        self.assertEqual(event['page'], 2)
        self.assertEqual(self.renderer.render.call_count, 1)

    def test_navigation_during_native_call_discards_completed_old_tile(self):
        def render(graphic, path, *args):
            path.write_bytes(b'png')
            self.current = self.second
        self.renderer.render.side_effect = render
        self.tiles.tick(0)
        self.assertIsNone(self.tiles.tick(1))
        self.tiles.install.assert_not_called()
        self.assertEqual(self.tiles.metrics['stale_discarded'], 1)
        self.tiles.tick(2)
        self.renderer.render.side_effect = lambda graphic, path, *args: path.write_bytes(b'png')
        self.assertEqual(self.tiles.tick(3)['page'], 2)

    def test_old_bridge_falls_back_once_without_removing_base_preview(self):
        self.renderer.render.side_effect = TileBridgeUnavailable('old Office')
        self.tiles.tick(0)
        self.assertEqual(self.tiles.tick(1)['event'], 'tiles-unavailable')
        self.assertIsNone(self.tiles.tick(2))
        self.assertEqual(self.renderer.render.call_count, 1)
        self.tiles.install.assert_not_called()
        self.assertEqual(self.tiles.metrics['rendered'], 0)

    def test_overlay_failure_clears_only_owned_tiles_and_stops_retrying(self):
        self.tiles.install.side_effect = RuntimeError('Cannot attach tile')
        self.tiles.tick(0)
        self.assertEqual(self.tiles.tick(1)['event'], 'tiles-failed')
        self.assertTrue(self.tiles.disabled)
        self.tiles.remove_except.assert_called_with(set())
        self.assertIsNone(self.tiles.tick(2))
        self.assertEqual(self.tiles.metrics['published'], 0)


if __name__ == '__main__':
    unittest.main()
