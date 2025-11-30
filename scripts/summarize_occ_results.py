#!/usr/bin/env python3
"""
summarize_occ_results.py

Scan OCC experiment result directories for metadata.json files and emit a CSV
summary combining scenario info and extracted metrics.
"""

import argparse
import csv
import json
import sys
from pathlib import Path


def gather_metadata(results_root):
    rows = []
    for metadata_path in results_root.glob("*/**/metadata.json"):
        try:
            with metadata_path.open("r", encoding="utf-8") as f:
                meta = json.load(f)
        except (OSError, json.JSONDecodeError):
            continue
        metrics = meta.get("metrics", {})
        row = {
            "session": metadata_path.parts[-3],
            "scenario": meta.get("name"),
            "description": meta.get("description", ""),
            "mode": meta.get("mode"),
            "timestamp": meta.get("timestamp"),
        }
        row["_has_metrics"] = bool(metrics)
        for key, value in metrics.items():
            row[key] = value
        rows.append(row)
    return rows


def main():
    parser = argparse.ArgumentParser(description="Summarize OCC experiment results.")
    parser.add_argument(
        "--results-root",
        type=Path,
        default=Path("/home/azureuser/mako-eocc/results/occ_runs"),
        help="Root directory containing OCC experiment runs.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="Optional CSV output file (defaults to stdout).",
    )
    parser.add_argument(
        "--latest",
        action="store_true",
        help="Only include scenarios from the latest session directory.",
    )
    parser.add_argument(
        "--include-empty",
        action="store_true",
        help="Include rows without parsed metrics (default: skip).",
    )
    args = parser.parse_args()

    results_root = args.results_root.expanduser().resolve()
    if not results_root.exists():
        parser.error(f"Results root not found: {results_root}")

    rows = gather_metadata(results_root)
    if not rows:
        print("No metadata files found.", file=sys.stderr)
        return

    if args.latest:
        latest_session = max(row["session"] for row in rows if row.get("session"))
        rows = [row for row in rows if row.get("session") == latest_session]

    fieldnames = [
        "session",
        "scenario",
        "description",
        "mode",
        "timestamp",
        "duration_seconds",
        "committed",
        "aborted",
        "abort_rate",
        "throughput_tps",
        "runtime_seconds",
        "n_commits",
        "agg_throughput_tps",
        "agg_abort_rate_per_sec",
        "avg_latency_ms",
    ]

    out_file = args.output.open("w", newline="", encoding="utf-8") if args.output else sys.stdout
    should_close = args.output is not None
    try:
        writer = csv.DictWriter(out_file, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            if not args.include_empty and not row.get("_has_metrics"):
                continue
            cleaned = {k: row.get(k, "") for k in fieldnames}
            writer.writerow(cleaned)
    finally:
        if should_close:
            out_file.close()


if __name__ == "__main__":
    main()

