#!/usr/bin/env python3
"""
Script to analyze OCC analysis results and generate reports
"""

import os
import sys
import json
import csv
from pathlib import Path
from collections import defaultdict
import statistics

def parse_log_file(log_file):
    """Parse log file to extract metrics"""
    metrics = {
        'total_validations': 0,
        'total_aborts': 0,
        'total_commits': 0,
        'abort_rate': 0.0,
        'false_abort_rate': 0.0,
    }
    
    if not os.path.exists(log_file):
        return metrics
    
    with open(log_file, 'r') as f:
        for line in f:
            if 'Total Validations:' in line:
                metrics['total_validations'] = int(line.split(':')[1].strip())
            elif 'Total Aborts:' in line:
                metrics['total_aborts'] = int(line.split(':')[1].strip())
            elif 'Total Commits:' in line:
                metrics['total_commits'] = int(line.split(':')[1].strip())
            elif 'Abort Rate:' in line:
                try:
                    metrics['abort_rate'] = float(line.split(':')[1].strip().rstrip('%'))
                except:
                    pass
            elif 'False Abort Rate:' in line:
                try:
                    metrics['false_abort_rate'] = float(line.split(':')[1].strip().rstrip('%'))
                except:
                    pass
    
    return metrics

def parse_performance_csv(csv_file):
    """Parse performance CSV file"""
    if not os.path.exists(csv_file):
        return {}
    
    validation_times = []
    version_check_times = []
    lock_times = []
    abort_count = 0
    
    with open(csv_file, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            validation_times.append(int(row.get('validation_time_ns', 0)))
            version_check_times.append(int(row.get('version_check_time_ns', 0)))
            lock_time = int(row.get('read_lock_time_ns', 0)) + int(row.get('write_lock_time_ns', 0))
            lock_times.append(lock_time)
            if row.get('aborted', '0') == '1':
                abort_count += 1
    
    if not validation_times:
        return {}
    
    def percentile(data, p):
        sorted_data = sorted(data)
        idx = int(len(sorted_data) * p)
        return sorted_data[min(idx, len(sorted_data) - 1)]
    
    return {
        'validation_time_p50': percentile(validation_times, 0.50),
        'validation_time_p95': percentile(validation_times, 0.95),
        'validation_time_p99': percentile(validation_times, 0.99),
        'validation_time_avg': statistics.mean(validation_times),
        'version_check_time_p50': percentile(version_check_times, 0.50),
        'version_check_time_p95': percentile(version_check_times, 0.95),
        'version_check_time_p99': percentile(version_check_times, 0.99),
        'lock_time_p50': percentile(lock_times, 0.50),
        'lock_time_p95': percentile(lock_times, 0.95),
        'lock_time_p99': percentile(lock_times, 0.99),
        'abort_count': abort_count,
    }

def generate_report(results_dir):
    """Generate analysis report"""
    results_dir = Path(results_dir)
    
    if not results_dir.exists():
        print(f"Error: Results directory does not exist: {results_dir}")
        return
    
    report = {
        'summary': {},
        'scenarios': {},
    }
    
    scenarios = ['low_contention', 'medium_contention', 'high_contention', 'hotspot']
    
    for scenario in scenarios:
        log_file = results_dir / f"{scenario}.log"
        perf_file = results_dir / f"{scenario}_perf.csv"
        stats_file = results_dir / f"{scenario}_stats.txt"
        
        scenario_data = {
            'metrics': parse_log_file(log_file),
            'performance': parse_performance_csv(perf_file),
        }
        
        report['scenarios'][scenario] = scenario_data
    
    # Generate summary
    print("\n" + "="*60)
    print("OCC Analysis Report")
    print("="*60 + "\n")
    
    print("Scenario Comparison:")
    print("-" * 60)
    print(f"{'Scenario':<20} {'Abort Rate':<15} {'False Abort %':<15} {'P99 Latency (ns)':<20}")
    print("-" * 60)
    
    for scenario in scenarios:
        data = report['scenarios'][scenario]
        abort_rate = data['metrics'].get('abort_rate', 0.0)
        false_abort_rate = data['metrics'].get('false_abort_rate', 0.0)
        p99_latency = data['performance'].get('validation_time_p99', 0)
        
        print(f"{scenario:<20} {abort_rate:<15.2f} {false_abort_rate:<15.2f} {p99_latency:<20}")
    
    print("\n" + "="*60)
    print("Key Findings:")
    print("="*60)
    
    # Find bottlenecks
    max_abort_rate = max(
        (report['scenarios'][s]['metrics'].get('abort_rate', 0) for s in scenarios),
        default=0
    )
    max_scenario = next(
        (s for s in scenarios if report['scenarios'][s]['metrics'].get('abort_rate', 0) == max_abort_rate),
        None
    )
    
    if max_scenario:
        print(f"1. Highest abort rate: {max_abort_rate:.2f}% ({max_scenario})")
    
    # Find false abort opportunities
    total_false_aborts = sum(
        report['scenarios'][s]['metrics'].get('false_abort_rate', 0) 
        for s in scenarios
    )
    avg_false_abort_rate = total_false_aborts / len(scenarios) if scenarios else 0
    
    print(f"2. Average false abort rate: {avg_false_abort_rate:.2f}%")
    print(f"   → Potential improvement with batching/reordering")
    
    # Performance bottlenecks
    print("\n3. Performance Bottlenecks:")
    for scenario in scenarios:
        perf = report['scenarios'][scenario]['performance']
        if perf:
            version_check_p99 = perf.get('version_check_time_p99', 0)
            lock_p99 = perf.get('lock_time_p99', 0)
            validation_p99 = perf.get('validation_time_p99', 0)
            
            if validation_p99 > 0:
                version_pct = (version_check_p99 / validation_p99) * 100
                lock_pct = (lock_p99 / validation_p99) * 100
                
                print(f"   {scenario}:")
                print(f"     - Version check: {version_pct:.1f}% of validation time")
                print(f"     - Lock acquisition: {lock_pct:.1f}% of validation time")
    
    # Save JSON report
    json_file = results_dir / "analysis_report.json"
    with open(json_file, 'w') as f:
        json.dump(report, f, indent=2)
    
    print(f"\nDetailed report saved to: {json_file}")
    print("="*60 + "\n")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 analyze_occ_results.py <results_directory>")
        sys.exit(1)
    
    results_dir = sys.argv[1]
    generate_report(results_dir)

