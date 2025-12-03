#!/usr/bin/env python3
"""
Combine OCC scaling runs from results/occ_runs/final and plot metrics vs. cores.
"""

import argparse
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

import matplotlib.pyplot as plt

SCENARIOS: List[Tuple[str, str]] = [
    ("dbtest_baseline_tpcc", "Baseline (sequential)"),
    ("dbtest_batch_validation_tpcc", "Batch validation"),
    ("dbtest_batch_validation_reorder_minid", "Batch + reorder (min-id)"),
    ("dbtest_batch_validation_reorder_prod", "Batch + reorder (prod)"),
    ("dbtest_batch_validation_storage", "Batch + storage reorder"),
]


def _find_run_dir(base_dir: Path, core_count: int) -> Optional[Path]:
    """
    Locate the *_full run directory for a given core count.
    Prefers directories under final/*/<core>c/<timestamp>_full.
    """
    core_pattern = f"{core_count}c"
    core_dirs = sorted(d for d in base_dir.rglob(core_pattern) if d.is_dir())
    for core_dir in core_dirs:
        run_dirs = sorted(
            d for d in core_dir.iterdir() if d.is_dir() and d.name.endswith("_full")
        )
        if run_dirs:
            # Use latest run for this core count.
            return run_dirs[-1]
    return None


@dataclass
class MetricPoint:
    cores: int
    throughput: Optional[float]
    latency: Optional[float]
    abort_per_k: Optional[float]


def _load_metrics(base_dir: Path, cores: Iterable[int]):
    dataset: Dict[str, List[MetricPoint]] = {name: [] for name, _ in SCENARIOS}
    for core in cores:
        run_dir = _find_run_dir(base_dir, core)
        if run_dir is None:
            continue
        for scenario, _label in SCENARIOS:
            meta_path = run_dir / scenario / "metadata.json"
            if not meta_path.exists():
                continue
            metadata = json.loads(meta_path.read_text())
            metrics = metadata.get("metrics", {})
            throughput = metrics.get("agg_throughput_tps") or metrics.get("throughput_tps")
            latency = metrics.get("avg_latency_ms") or metrics.get("latency_ms")
            abort_rate = (
                metrics.get("agg_abort_rate_per_sec")
                or metrics.get("abort_rate_per_sec")
                or metrics.get("abort_rate")
            )
            runtime = metrics.get("runtime_seconds")
            n_commits = metrics.get("n_commits")
            if throughput is None and runtime and runtime > 0 and n_commits is not None:
                throughput = n_commits / runtime
            abort_per_k = None
            if throughput and throughput > 0 and abort_rate is not None:
                abort_per_k = (abort_rate / throughput) * 1000.0
            dataset[scenario].append(
                MetricPoint(
                    cores=core,
                    throughput=throughput,
                    latency=latency,
                    abort_per_k=abort_per_k,
                )
            )
    return dataset


def _plot_metric(
    dataset: Dict[str, List[MetricPoint]],
    metric: str,
    ylabel: str,
    cores: List[int],
    outfile: Path,
) -> None:
    fig, ax = plt.subplots(figsize=(8.5, 4.8))
    plotted = False
    for scenario, label in SCENARIOS:
        points = sorted(dataset.get(scenario, []), key=lambda item: item.cores)
        xs = []
        ys = []
        for item in points:
            value = getattr(item, metric, None)
            if value is None:
                continue
            xs.append(item.cores)
            ys.append(value)
        if not xs:
            continue
        ax.plot(xs, ys, marker="o", linewidth=2.0, label=label)
        plotted = True

    if not plotted:
        plt.close(fig)
        raise RuntimeError(f"No data available for metric '{metric}'.")

    ax.set_xlabel("Number of cores")
    ax.set_ylabel(ylabel)
    ax.set_xticks(cores)
    ax.grid(True, linestyle="--", linewidth=0.5, alpha=0.7)
    ax.legend(loc="best")
    fig.tight_layout()
    outfile.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(outfile, dpi=150)
    plt.close(fig)


def _write_summary(
    dataset: Dict[str, List[MetricPoint]],
    outfile: Path,
) -> None:
    headers = [
        "scenario",
        "label",
        "cores",
        "throughput_txn_per_s",
        "avg_latency_ms",
        "abort_per_1000_txn",
    ]
    lines = [",".join(headers)]
    for scenario, label in SCENARIOS:
        for point in sorted(dataset.get(scenario, []), key=lambda item: item.cores):
            values = [
                scenario,
                label,
                str(point.cores),
                f"{point.throughput if point.throughput is not None else 'n/a'}",
                f"{point.latency if point.latency is not None else 'n/a'}",
                f"{point.abort_per_k if point.abort_per_k is not None else 'n/a'}",
            ]
            lines.append(",".join(values))
    outfile.write_text("\n".join(lines))


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Plot throughput/latency/abort scaling vs cores for OCC runs."
    )
    parser.add_argument(
        "--root",
        default="results/occ_runs/final",
        help="Directory containing per-core OCC runs (default: %(default)s)",
    )
    parser.add_argument(
        "--cores",
        nargs="+",
        type=int,
        default=[1, 2, 4, 8, 16],
        help="Core counts to include (default: %(default)s)",
    )
    parser.add_argument(
        "--output-dir",
        default=None,
        help="Directory to write plots/summary (default: same as --root)",
    )
    args = parser.parse_args()

    base_dir = Path(args.root).expanduser().resolve()
    if not base_dir.exists():
        raise FileNotFoundError(f"Root directory {base_dir} does not exist")

    cores = sorted({core for core in args.cores if core > 0})
    dataset = _load_metrics(base_dir, cores)
    output_dir = Path(args.output_dir).expanduser().resolve() if args.output_dir else base_dir
    output_dir.mkdir(parents=True, exist_ok=True)

    _plot_metric(dataset, "throughput", "txn/s", cores, output_dir / "throughput_scaling.png")
    _plot_metric(dataset, "latency", "avg latency (ms)", cores, output_dir / "latency_scaling.png")
    _plot_metric(
        dataset,
        "abort_per_k",
        "aborts per 1K txns",
        cores,
        output_dir / "abort_rate_scaling.png",
    )
    _write_summary(dataset, output_dir / "scaling_summary.csv")


if __name__ == "__main__":
    main()

