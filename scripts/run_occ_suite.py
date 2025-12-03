#!/usr/bin/env python3
"""
run_occ_suite.py

Utility to run a sequence of OCC experiments defined in a YAML file.
Supports "fast" vs "full" modes, env var injection, logging, and dry-run mode.
"""

import argparse
import datetime
import json
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

try:
    import yaml  # type: ignore
except ImportError as exc:  # pragma: no cover
    print("Missing dependency: PyYAML is required to run this script.", file=sys.stderr)
    raise

from plot_occ_results import generate_plots


def resolve_value(raw_value, mode):
    """Resolve per-mode dictionaries to a concrete value."""
    if isinstance(raw_value, dict):
        if mode in raw_value:
            return raw_value[mode]
        if "default" in raw_value:
            return raw_value["default"]
        raise ValueError(f"No value for mode '{mode}' in {raw_value}")
    return raw_value


def build_placeholders(placeholder_cfg, mode):
    placeholders = {}
    for key, value in (placeholder_cfg or {}).items():
        placeholders[key] = resolve_value(value, mode)
    return placeholders


def build_env(env_cfg, placeholders, mode):
    env = {}
    for key, value in (env_cfg or {}).items():
        resolved = resolve_value(value, mode)
        if isinstance(resolved, str):
            resolved = resolved.format(**placeholders)
        env[key] = str(resolved)
    return env


def stream_process(proc, log_file, prefix):
    """Stream process output to both stdout and log file."""
    assert proc.stdout is not None
    for raw_line in proc.stdout:
        line = raw_line.decode(errors="replace")
        log_file.write(line)
        log_file.flush()
        print(f"[{prefix}] {line}", end="")


def run_command(cmd_list, env, workdir, log_path, prefix):
    workdir_path = Path(workdir)
    workdir_path.mkdir(parents=True, exist_ok=True)
    with open(log_path, "w", encoding="utf-8") as log_file:
        log_file.write(f"# Command: {' '.join(cmd_list)}\n")
        log_file.write(f"# Working dir: {workdir_path}\n")
        log_file.flush()
        proc = subprocess.Popen(
            cmd_list,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            cwd=str(workdir_path),
            env=env,
        )
        try:
            stream_process(proc, log_file, prefix)
        finally:
            return_code = proc.wait()
        log_file.write(f"\n# Exit code: {return_code}\n")
        log_file.flush()
    return return_code


STRESS_PATTERNS = {
    "duration_seconds": re.compile(r"Duration:\s+[^\(]*\(([\d\.]+)\s*s\)"),
    "committed": re.compile(r"Committed:\s+(\d+)"),
    "aborted": re.compile(r"Aborted:\s+(\d+)"),
    "abort_rate": re.compile(r"Abort Rate:\s+([\d\.]+)%"),
    "throughput_tps": re.compile(r"Throughput:\s+([\d\.]+)\s+txns/sec"),
}

DBTEST_PATTERNS = {
    "runtime_seconds": re.compile(r"runtime:\s+([\d\.]+)\s+sec"),
    "n_commits": re.compile(r"n_commits:\s+(\d+)"),
    "agg_throughput_tps": re.compile(r"agg_throughput:\s+([\d\.]+)\s+ops/sec"),
    "agg_abort_rate_per_sec": re.compile(r"agg_abort_rate:\s+([\d\.]+)\s+aborts/sec"),
    "avg_latency_ms": re.compile(r"avg_latency:\s+([\d\.]+)\s+ms"),
}

COUNTER_PATTERN = re.compile(
    r"^([A-Za-z0-9_]+):\s+count=([0-9]+)"
    r"(?:,\s+max=([0-9Ee+.\-]+))?"
    r"(?:,\s+avg=([0-9Ee+.\-]+))?",
    re.MULTILINE,
)


def extract_metrics(log_path):
    metrics = {}
    counters = {}
    try:
        text = Path(log_path).read_text(encoding="utf-8", errors="ignore")
    except FileNotFoundError:
        return metrics, counters
    for pattern_dict in (STRESS_PATTERNS, DBTEST_PATTERNS):
        for key, pattern in pattern_dict.items():
            match = pattern.search(text)
            if match:
                value_str = match.group(1)
                try:
                    value = float(value_str)
                except ValueError:
                    continue
                metrics[key] = value
    for match in COUNTER_PATTERN.finditer(text):
        name = match.group(1)
        count_str = match.group(2)
        max_str = match.group(3)
        avg_str = match.group(4)
        try:
            count_val = int(count_str)
        except ValueError:
            continue
        if max_str is None and avg_str is None:
            counters[name] = count_val
            continue
        entry = {"count": count_val}
        if max_str is not None:
            try:
                entry["max"] = float(max_str)
            except ValueError:
                pass
        if avg_str is not None:
            try:
                entry["avg"] = float(avg_str)
            except ValueError:
                pass
        counters[name] = entry
    return metrics, counters


def main():
    parser = argparse.ArgumentParser(description="Run OCC experiment suite.")
    parser.add_argument(
        "--scenarios",
        type=Path,
        default=Path("/home/azureuser/mako-eocc/config/occ_experiments.yml"),
        help="Path to scenarios YAML file.",
    )
    parser.add_argument(
        "--mode",
        choices=("fast", "full"),
        default="fast",
        help="Mode that controls placeholder resolution.",
    )
    parser.add_argument(
        "--results-root",
        type=Path,
        default=Path("/home/azureuser/mako-eocc/results/occ_runs"),
        help="Directory to store experiment outputs.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print commands without executing them.",
    )
    parser.add_argument(
        "--only",
        nargs="*",
        help="Optional list of scenario names to run.",
    )
    args = parser.parse_args()

    scenarios_path = args.scenarios.expanduser().resolve()
    if not scenarios_path.exists():
        parser.error(f"Scenario file not found: {scenarios_path}")

    with open(scenarios_path, "r", encoding="utf-8") as f:
        suite_cfg = yaml.safe_load(f) or {}

    defaults = suite_cfg.get("defaults", {})
    workdir_default = Path(defaults.get("workdir", "/home/azureuser/mako-eocc"))
    results_root = args.results_root.expanduser().resolve()

    timestamp = datetime.datetime.utcnow().strftime("%Y%m%d-%H%M%S")
    session_dir = results_root / f"{timestamp}_{args.mode}"
    session_dir.mkdir(parents=True, exist_ok=True)

    scenarios = suite_cfg.get("scenarios", [])
    if not scenarios:
        parser.error("No scenarios defined in YAML file.")

    selected = set(args.only) if args.only else None
    ran_any = False

    for scenario in scenarios:
        name = scenario.get("name")
        if not name:
            print("Skipping unnamed scenario", file=sys.stderr)
            continue
        if selected and name not in selected:
            continue

        description = scenario.get("description", "")
        placeholders = build_placeholders(scenario.get("placeholders"), args.mode)
        try:
            command_template = scenario["command"]
        except KeyError as exc:
            raise ValueError(f"Scenario '{name}' missing 'command' field") from exc

        formatted_command = command_template.format(**placeholders)
        cmd_list = shlex.split(formatted_command)

        scenario_env = build_env(scenario.get("env"), placeholders, args.mode)
        merged_env = os.environ.copy()
        merged_env.update(scenario_env)

        scenario_dir = session_dir / name
        scenario_dir.mkdir(parents=True, exist_ok=True)
        log_path = scenario_dir / "stdout.log"
        metadata_path = scenario_dir / "metadata.json"

        metadata = {
            "name": name,
            "description": description,
            "mode": args.mode,
            "command_template": command_template,
            "command": cmd_list,
            "placeholders": placeholders,
            "env": scenario_env,
            "workdir": str(Path(scenario.get("workdir", workdir_default)).resolve()),
            "timestamp": timestamp,
            "dry_run": args.dry_run,
        }
        with open(metadata_path, "w", encoding="utf-8") as meta_file:
            json.dump(metadata, meta_file, indent=2)

        print(f"\n=== Scenario: {name} ({args.mode}) ===")
        if description:
            print(description)
        print("Command:", " ".join(cmd_list))
        print("Working dir:", metadata["workdir"])
        print("Env overrides:", scenario_env)
        print("Logs:", log_path)

        if args.dry_run:
            continue

        return_code = run_command(
            cmd_list,
            merged_env,
            metadata["workdir"],
            log_path,
            prefix=name,
        )
        if return_code != 0:
            print(f"Scenario '{name}' failed with exit code {return_code}", file=sys.stderr)
            break
        metrics, counters = extract_metrics(log_path)
        if metrics:
            metadata["metrics"] = metrics
            with open(metadata_path, "w", encoding="utf-8") as meta_file:
                json.dump(metadata, meta_file, indent=2)
        if counters:
            metadata.setdefault("event_counters", counters)
            with open(metadata_path, "w", encoding="utf-8") as meta_file:
                json.dump(metadata, meta_file, indent=2)
        ran_any = True

    if not args.dry_run and ran_any:
        try:
            print(f"\nGenerating plots for {session_dir} ...")
            generate_plots(session_dir)
            print("Plots saved to", session_dir)
        except Exception as exc:  # pragma: no cover
            print(f"Failed to generate plots for {session_dir}: {exc}", file=sys.stderr)


if __name__ == "__main__":
    main()

