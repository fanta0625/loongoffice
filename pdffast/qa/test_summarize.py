#!/usr/bin/env python3
"""Log-format regression checks; no LibreOffice runtime required."""
import unittest

from summarize import summarize


class SummarizeTest(unittest.TestCase):
    def test_native_stages_and_retries(self):
        result = summarize("""
info:pdffast:1: native-import total_us=900
info:pdffast:1: native-source-snapshot bytes=1234
info:pdffast:1: native-edit-session opened
info:pdffast:1: native-edit-transport page=0 success=1 total_us=70
info:pdffast:1: native-edit-import backend=poppler pdf_page=0 success=1 pages=1 total_us=20
info:pdffast:1: native-edit-prepare page=0 objects=3 success=1 total_us=30
info:pdffast:1: native-edit-transport page=999 success=0 total_us=5
info:pdffast:1: native-edit-session opened
info:pdffast:1: native-edit-transport page=1 success=1 total_us=40
info:pdffast:1: native-edit-transport page=2 success=0
""")
        self.assertEqual(result["native_helper_opens"], 2)
        self.assertEqual(result["native_source_snapshots_bytes"], [1234])
        stages = result["native_stages"]
        self.assertEqual(stages["edit-transport"]["timing"],
                         {"count": 3, "sum_us": 115, "p50_us": 40,
                          "p95_us": 70, "p99_us": 70, "max_us": 70})
        self.assertEqual(stages["edit-transport"]["records"][1]["success"], 0)
        self.assertEqual(stages["edit-import"]["records"][0]["pdf_page"], 0)
        self.assertEqual(stages["edit-prepare"]["timing"]["sum_us"], 30)
        self.assertEqual(stages["import"]["timing"]["sum_us"], 900)

    def test_empty_and_existing_formats(self):
        self.assertEqual(summarize("")["native_stages"]["edit-import"]["timing"], {"count": 0})
        result = summarize("""
native-ui-image page=1 allocation_us=2 upload_cache_us=4
UI result batch 3 items in 6 us
PDF region children=2 aborted=0 in 8 us
input-summary reads=1 checks=2 misses=3 bytes=4 read_us=5 check_us=6
""")
        self.assertEqual(result["ui_allocation"]["sum_us"], 2)
        self.assertEqual(result["ui_upload_and_cache"]["sum_us"], 4)
        self.assertEqual(result["ui_result_batch"]["sum_us"], 6)
        self.assertEqual(result["render_region"]["sum_us"], 8)
        self.assertEqual(result["input_sessions"][0]["checks"], 2)

    def test_deferred_metadata_and_native_snapshot(self):
        stages = summarize("""
native-metadata page=7 annotations=2 links=1 total_us=21
native-read-snapshot page=15 width=512 height=217 total_us=900
native-metadata page=8 annotations=0 links=0 total_us=2
""")["native_stages"]
        self.assertEqual(stages["metadata"]["timing"]["sum_us"], 23)
        self.assertEqual(stages["metadata"]["records"][0],
                         {"page": 7, "annotations": 2, "links": 1, "total_us": 21})
        self.assertEqual(stages["read-snapshot"]["records"][0],
                         {"page": 15, "width": 512, "height": 217, "total_us": 900})


if __name__ == "__main__":
    unittest.main()
