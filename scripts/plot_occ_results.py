#!/usr/bin/env python3
import argparse
import json
import math
from pathlib import Path
from typing import Dict, List, Optional

import matplotlib.pyplot as plt


def load_run_data(run_dir: Path) -> List[Dict[str, Optional[float]]]:
    scenarios = []
    for scenario_path in sorted(run_dir.iterdir()):
        if not scenario_path.is_dir():
            continue
        meta_path = scenario_path / "metadata.json"
        if not meta_path.exists():
            continue
        try:
            metadata = json.loads(meta_path.read_text())
        except json.JSONDecodeError:
            continue
        metrics = metadata.get("metrics", {})
        value = {
            "scenario": scenario_path.name,
            "throughput": metrics.get("agg_throughput_tps")
            or metrics.get("throughput_tps"),
            "latency": metrics.get("avg_latency_ms") or metrics.get("latency_ms"),
            "abort_rate": metrics.get("agg_abort_rate_per_sec")
            or metrics.get("abort_rate"),
        }
        # Skip entries with no metrics at all.
        if all(v is None for k, v in value.items() if k != "scenario"):
            continue
        scenarios.append(value)

    if not scenarios:
        raise ValueError(f"No scenario metadata found in {run_dir}")

    def sort_key(entry: Dict[str, Optional[float]]):
        name = entry["scenario"]
        return (0 if "baseline" in name else 1, name)

    scenarios.sort(key=sort_key)
    return scenarios


def _format_value(num: Optional[float]) -> str:
    if num is None or math.isnan(num):
        return "n/a"
    if abs(num) >= 100:
        return f"{num:,.0f}"
    if abs(num) >= 1:
        return f"{num:,.2f}"
    return f"{num:.4f}"


def plot_metric(
    rows: List[Dict[str, Optional[float]]],
    field: str,
    title: str,
    ylabel: str,
    outfile: Path,
) -> None:
    filtered = [
        (row["scenario"], row[field])
        for row in rows
        if row.get(field) is not None
    ]
    if not filtered:
        return

    names, values = zip(*filtered)

    width = max(8.0, len(names) * 0.65)
    fig, ax = plt.subplots(figsize=(width, 4.8))
    bar_positions = range(len(names))
    bars = ax.bar(bar_positions, values, color="#4C72B0")
    ax.set_title(title)
    ax.set_ylabel(ylabel)
    ax.set_xticks(list(bar_positions))
    ax.set_xticklabels(names, rotation=60, ha="right")
    ax.grid(axis="y", linestyle="--", linewidth=0.4, alpha=0.6)

    min_val = min(values)
    max_val = max(values)
    if math.isclose(min_val, max_val):
        delta = 0.1 * (abs(max_val) if max_val != 0 else 1.0)
        bottom = min_val - delta
        top = max_val + delta
    else:
        bottom = min_val
        top = max_val
    ax.set_ylim(bottom, top)

    for bar, value in zip(bars, values):
        height = bar.get_height()
        ax.text(
            bar.get_x() + bar.get_width() / 2.0,
            height,
            _format_value(value),
            ha="center",
            va="bottom",
            fontsize=8,
            rotation=90,
        )

    fig.tight_layout()
    fig.savefig(outfile, dpi=150)
    plt.close(fig)


def write_summary(rows: List[Dict[str, Optional[float]]], outfile: Path) -> None:
    headers = ["scenario", "throughput_txn_per_s", "avg_latency_ms", "abort_rate_per_s"]
    lines = [",".join(headers)]
    for row in rows:
        lines.append(
            ",".join(
                [
                    row["scenario"],
                    _format_value(row.get("throughput")),
                    _format_value(row.get("latency")),
                    _format_value(row.get("abort_rate")),
                ]
            )
        )
    outfile.write_text("\n".join(lines))


def generate_plots(run_dir: Path, show: bool = False) -> None:
    run_dir = run_dir.expanduser().resolve()
    if not run_dir.exists():
        raise FileNotFoundError(f"Run directory {run_dir} does not exist")

    rows = load_run_data(run_dir)

    plot_rows = [
        row
        for row in rows
        if row["scenario"].startswith("dbtest")
        and "inline" not in row["scenario"]
    ]
    if not plot_rows:
        plot_rows = rows

    plot_metric(
        plot_rows,
        "throughput",
        "Throughput (txn/s)",
        "txn/s",
        run_dir / "throughput.png",
    )
    plot_metric(
        plot_rows, "latency", "Average Latency (ms)", "ms", run_dir / "latency.png"
    )
    plot_metric(
        plot_rows,
        "abort_rate",
        "Abort Rate",
        "aborts per second",
        run_dir / "abort_rate.png",
    )

    write_summary(rows, run_dir / "summary_metrics.csv")

    if show:
        plt.show()


def main():
    parser = argparse.ArgumentParser(
        description="Plot throughput/latency/abort rate for OCC runs."
    )
    parser.add_argument(
        "--run",
        required=True,
        help="Path to results/occ_runs/<timestamp> directory",
    )
    parser.add_argument(
        "--show",
        action="store_true",
        help="Display plots interactively instead of saving",
    )
    args = parser.parse_args()
    generate_plots(Path(args.run), show=args.show)


if __name__ == "__main__":
    main()

