# OCC Analysis Tools - Usage Guide

This guide explains how to use the OCC analysis tools to measure bottlenecks, abort rates, and false aborts in Mako's current OCC implementation.

## Overview

The analysis tools consist of:
1. **Analysis Infrastructure** (`occ_analysis.h/cc`): Core tracking and profiling classes
2. **Instrumented Scheduler** (`scheduler_instrumented.cc`): Instrumented version of OCC scheduler
3. **Analysis Scripts**: Scripts to run tests and analyze results

## Quick Start

### Step 1: Enable Analysis in Build

Add analysis instrumentation to the OCC scheduler. You have two options:

#### Option A: Conditional Compilation (Recommended)

Modify `src/deptran/occ/scheduler.cc` to include analysis when enabled:

```cpp
// At the top of scheduler.cc, add:
#ifdef OCC_ANALYSIS_ENABLED
#include "occ_analysis.h"
#endif

// In DoPrepare(), add instrumentation:
bool SchedulerOcc::DoPrepare(txnid_t tx_id) {
#ifdef OCC_ANALYSIS_ENABLED
    auto& profiler = PerformanceProfiler::getInstance();
    profiler.startValidation(tx_id);
#endif
    
    // ... existing code ...
    
#ifdef OCC_ANALYSIS_ENABLED
    if (version_check_failed) {
        profiler.endValidation(tx_id, false);
        AbortTracker::getInstance().recordAbort(tx_id, OCCAbortReason::VERSION_CONFLICT);
        return false;
    }
#endif
    
    // ... rest of code ...
}
```

#### Option B: Replace Scheduler File

Temporarily replace `scheduler.cc` with `scheduler_instrumented.cc`:

```bash
cd src/deptran/occ
mv scheduler.cc scheduler_original.cc
cp scheduler_instrumented.cc scheduler.cc
```

### Step 2: Build with Analysis Enabled

```bash
cd build
cmake -DOCC_ANALYSIS_ENABLED=ON ..
make -j$(nproc) dbtest
cd ..
```

### Step 3: Run Analysis

```bash
# Run analysis with varying contention levels
./scripts/run_occ_analysis.sh config/occ.yml

# Or run manually with specific parameters
./build/dbtest --bench tpcc --config config/occ.yml \
    --num-threads 48 --runtime 60
```

### Step 4: Analyze Results

```bash
# Analyze results from the last run
python3 scripts/analyze_occ_results.py results/occ_analysis_*/

# Or specify directory
python3 scripts/analyze_occ_results.py results/occ_analysis_20240101_120000
```

## Manual Usage

### Using Analysis Tools in Code

```cpp
#include "deptran/occ/occ_analysis.h"

// In your code:

// 1. Track aborts
auto& abort_tracker = AbortTracker::getInstance();
abort_tracker.recordAbort(txn_id, OCCAbortReason::VERSION_CONFLICT, 
                          AbortType::FALSE_ABORT);

// 2. Profile performance
auto& profiler = PerformanceProfiler::getInstance();
profiler.startValidation(txn_id);
// ... validation code ...
profiler.endValidation(txn_id, success);

// 3. Analyze false aborts
auto& analyzer = FalseAbortAnalyzer::getInstance();
analyzer.recordReadWriteSets(txn_id, read_set, write_set);
bool could_reorder = analyzer.analyzeAbort(txn1, txn2, conflicting_keys);
```

### Printing Statistics

```cpp
// Print to console
AbortTracker::getInstance().printStatistics();
PerformanceProfiler::getInstance().printStatistics();
FalseAbortAnalyzer::getInstance().printAnalysis();

// Write to files
AbortTracker::getInstance().writeStatisticsToFile("abort_stats.txt");
PerformanceProfiler::getInstance().writeStatisticsToFile("perf_stats.csv");
FalseAbortAnalyzer::getInstance().writeAnalysisToFile("false_aborts.csv");
```

## What Gets Measured

### 1. Performance Bottlenecks

- **Validation Time**: Total time spent in `DoPrepare()`
- **Version Check Time**: Time spent checking versions
- **Lock Acquisition Time**: Time acquiring read/write locks
- **Percentiles**: P50, P95, P99 latencies

### 2. Abort Rates

- **Total Aborts**: Number of aborted transactions
- **Abort Rate**: Percentage of transactions aborted
- **Abort Reasons**: 
  - Version conflicts
  - Read lock failures
  - Write lock failures
- **Abort Types**:
  - Real conflicts (must abort)
  - False aborts (could be avoided)
  - Version stale (read stale data)
  - Ordering issues (wrong validation order)
  - Lock contention

### 3. False Abort Analysis

- **Detectable False Aborts**: Aborts that could be avoided with reordering
- **Conflict Information**: Which transactions conflict and why
- **Reorderability**: Whether transactions could commit in different order

## Output Files

After running analysis, you'll get:

```
results/occ_analysis_YYYYMMDD_HHMMSS/
├── low_contention.log          # Log output
├── low_contention_perf.csv     # Performance metrics (CSV)
├── low_contention_stats.txt    # Statistics summary
├── medium_contention.log
├── medium_contention_perf.csv
├── high_contention.log
├── hotspot.log
├── summary.md                  # Summary report
└── analysis_report.json        # Detailed JSON report
```

## Interpreting Results

### Abort Rate Analysis

```
Abort Rate: 15.3%
False Abort Rate: 8.2%
```

**Interpretation**:
- 15.3% of transactions abort
- 8.2% of aborts could potentially be avoided with batching/reordering
- **Opportunity**: ~8.2% improvement possible

### Performance Bottlenecks

```
Validation Time P99: 50000 ns
Version Check Time P99: 30000 ns (60%)
Lock Time P99: 15000 ns (30%)
```

**Interpretation**:
- Version checking takes 60% of validation time → **Bottleneck**
- Lock acquisition takes 30% → Moderate overhead
- **Opportunity**: Optimize version checking (batching could help)

### False Abort Analysis

```
False Aborts: 120 / 500 total aborts (24%)
```

**Interpretation**:
- 24% of aborts are false aborts
- These could be avoided with:
  - Transaction batching
  - Operation reordering
  - Better validation order

## Test Scenarios

### Low Contention
- **Purpose**: Baseline performance
- **Config**: Uniform key distribution, low concurrency
- **Expected**: Low abort rate, good performance

### Medium Contention
- **Purpose**: Moderate conflict scenario
- **Config**: Zipfian distribution (theta=0.5), medium concurrency
- **Expected**: Moderate abort rate

### High Contention
- **Purpose**: Stress test
- **Config**: Zipfian distribution (theta=0.9), high concurrency
- **Expected**: High abort rate, performance degradation

### Hotspot
- **Purpose**: Extreme contention
- **Config**: Highly skewed (theta=0.99), many threads
- **Expected**: Very high abort rate, many false aborts

## Next Steps After Analysis

1. **Identify Bottlenecks**: Look at performance breakdown
2. **Measure False Aborts**: See how many could be avoided
3. **Plan Enhancements**: Based on findings:
   - High false abort rate → Implement batching
   - Version check bottleneck → Optimize version checking
   - Lock contention → Consider hybrid approach
4. **Set Targets**: Define improvement goals (e.g., reduce abort rate by 30%)

## Troubleshooting

### Analysis Not Enabled

**Symptom**: No statistics printed

**Solution**: 
- Check that `OCC_ANALYSIS_ENABLED` is defined
- Verify `occ_analysis.h` is included
- Check that instrumentation code is present

### Missing Output Files

**Symptom**: CSV/stats files not created

**Solution**:
- Check file permissions
- Verify output directory exists
- Check that analysis tools are called (add print statements)

### Performance Overhead

**Symptom**: Significant slowdown with analysis enabled

**Solution**:
- Analysis adds ~5-10% overhead
- Use sampling (only analyze every Nth transaction)
- Disable detailed tracking for production runs

## Example Output

```
========== OCC Abort Statistics ==========
Total Validations: 1000000
Total Commits: 847000
Total Aborts: 153000
Abort Rate: 15.30%

Abort Reasons:
  VERSION_CONFLICT: 92000
  READ_LOCK_FAILED: 35000
  WRITE_LOCK_FAILED: 26000

Abort Types:
  Real Conflicts: 92000
  False Aborts: 45000
  Version Stale: 18000

False Abort Rate: 45000 / 153000 (29.41%)
==========================================

========== OCC Performance Statistics ==========
Validation Time (ns):
  P50: 5000
  P95: 25000
  P99: 50000
  Avg: 8000

Version Check Time (ns):
  P50: 3000
  P95: 15000
  P99: 30000

Lock Acquisition Time (ns):
  P50: 1000
  P95: 8000
  P99: 15000
================================================
```

## Integration with Existing Code

The analysis tools are designed to be **non-intrusive**:
- Use singleton pattern (getInstance())
- Thread-safe (mutex-protected)
- Optional (can be disabled via compile flag)
- Low overhead when enabled

You can gradually add instrumentation without breaking existing functionality.

