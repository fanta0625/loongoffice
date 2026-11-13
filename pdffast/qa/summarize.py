#!/usr/bin/env python3
"""Summarize SAL_LOG=+INFO.pdffast logs; standard library only."""
import argparse
import json
import math
import re
from pathlib import Path


def distribution(values):
    values = sorted(values)
    if not values:
        return {"count": 0}
    return {"count": len(values), "sum_us": sum(values),
            "p50_us": values[math.ceil(len(values) * .50) - 1],
            "p95_us": values[math.ceil(len(values) * .95) - 1],
            "p99_us": values[math.ceil(len(values) * .99) - 1],
            "max_us": values[-1]}


def summarize(text):
    allocations, uploads, batches, regions, inputs = [], [], [], [], []
    stages = ("import", "edit-transport", "edit-import", "edit-prepare", "metadata", "read-snapshot")
    native = {stage: [] for stage in stages}
    snapshots, helper_opens = [], 0
    for line in text.splitlines():
        image = re.search(r"ui-image .*allocation_us=(\d+) upload_cache_us=(\d+)", line)
        if image:
            allocations.append(int(image[1]))
            uploads.append(int(image[2]))
        batch = re.search(r"UI result batch \d+ items in (\d+) us", line)
        if batch:
            batches.append(int(batch[1]))
        region = re.search(r"PDF region children=\d+ aborted=\d+ in (\d+) us", line)
        if region:
            regions.append(int(region[1]))
        if "input-summary " in line:
            inputs.append({key: int(value) for key, value in
                           re.findall(r"(reads|checks|misses|bytes|read_us|check_us)=(\d+)", line)})
        stage = re.search(r"\bnative-(" + "|".join(stages) + r") (.*)", line)
        if stage:
            fields = {key: int(value) for key, value in
                      re.findall(r"\b(page|pdf_page|success|pages|objects|annotations|links|width|height|total_us)=(-?\d+)", stage[2])}
            if "total_us" in fields:
                native[stage[1]].append(fields)
        snapshot = re.search(r"\bnative-source-snapshot bytes=(\d+)", line)
        if snapshot:
            snapshots.append(int(snapshot[1]))
        if "native-edit-session opened" in line:
            helper_opens += 1
    return {"ui_allocation": distribution(allocations),
            "ui_upload_and_cache": distribution(uploads),
            "ui_result_batch": distribution(batches), "render_region": distribution(regions),
            "input_sessions": inputs,
            "native_stages": {
                stage: {"timing": distribution([record["total_us"] for record in records]),
                        "records": records}
                for stage, records in native.items()},
            "native_source_snapshots_bytes": snapshots,
            "native_helper_opens": helper_opens,
            "note": "CPU-stage wall times, not frame presentation or cold-start latency. "
                    "read_us includes nested checks; check_us also includes checks outside reads. "
                    "Native edit-import is nested in edit-prepare; transport runs separately. "
                    "Snapshot/helper counts aggregate all documents and retries in the log. "
                    "Do not add these totals or interpret their ratio as a percentage."}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    print(json.dumps(summarize(args.log.read_text(encoding="utf-8", errors="replace")), indent=2))
