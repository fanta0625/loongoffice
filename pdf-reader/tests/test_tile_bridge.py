# SPDX-License-Identifier: MPL-2.0
from pathlib import Path
import sys
import unittest
import tempfile
from types import SimpleNamespace
from unittest.mock import Mock, patch
import struct
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'uno' / 'python'))
from tile_bridge import validate_tile, tile_placement, InternalTileRenderer, TileBridgeUnavailable


class TileGeometryTests(unittest.TestCase):
    def test_old_provider_ignoring_options_cannot_pass_capability_check(self):
        renderer = object.__new__(InternalTileRenderer)
        renderer.checked = False
        renderer._export = Mock()
        class InvalidArgument(Exception):
            pass
        with tempfile.TemporaryDirectory() as directory, patch.dict(
                sys.modules, {'uno': SimpleNamespace(getClass=lambda name: InvalidArgument)}):
            with self.assertRaises(TileBridgeUnavailable):
                renderer.render(object(), Path(directory) / 'tile.png', (1000, 1000), (0, 0, 512, 512))
            self.assertFalse(renderer.checked)

    def test_capability_acknowledgement_then_exact_tile_dimensions(self):
        renderer = object.__new__(InternalTileRenderer)
        renderer.checked = False
        class InvalidArgument(Exception):
            pass
        def export(graphic, path, size, rect):
            if rect[2] == 0:
                raise InvalidArgument('PDF tile: invalid bounds')
            # The adapter checks dimensions; full PNG decoding belongs to the
            # integration probe, not this transport contract test.
            Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + b'\0\0\0\rIHDR' + struct.pack('>II', *rect[2:]))
        renderer._export = export
        with tempfile.TemporaryDirectory() as directory, patch.dict(
                sys.modules, {'uno': SimpleNamespace(getClass=lambda name: InvalidArgument)}):
            path = Path(directory) / 'tile.png'
            self.assertEqual(renderer.render(object(), path, (1000, 1000), (0, 0, 512, 512)), path)
            self.assertTrue(renderer.checked)

    def test_neighbour_edges_match_on_fractional_page_scale(self):
        a = tile_placement((3333, 4714), (0, 512, 512, 512), (120, 130, 21001, 29703))
        b = tile_placement((3333, 4714), (512, 512, 512, 512), (120, 130, 21001, 29703))
        self.assertEqual(a[0] + a[2], b[0])
        self.assertEqual(a[1], b[1])

    def test_edge_tile_ends_at_page_boundary(self):
        a = tile_placement((1000, 2000), (512, 1536, 488, 464), (0, 0, 21000, 29700))
        self.assertEqual(a[0] + a[2], 21000)
        self.assertEqual(a[1] + a[3], 29700)

    def test_invalid_or_unbounded_requests_rejected(self):
        for size, rect in (((0, 100), (0, 0, 1, 1)), ((100, 100), (-1, 0, 1, 1)),
                           ((100, 100), (99, 0, 2, 1)), ((5000, 5000), (0, 0, 4096, 1)),
                           ((100, 100), (0, 0, 0, 1)), ((200000, 100), (0, 0, 1, 1))):
            with self.assertRaises(ValueError):
                validate_tile(size, rect)
