# How to Test Batch Validation Performance

## Quick Answer

**Option 1: Use the Automated Test Script (Easiest)**

```bash
# Run the performance test script
./run_performance_test.sh
```

This will:
1. Build a stress test (if needed)
2. Run baseline test (sequential validation)
3. Run with batch validation enabled
4. Compare throughput and show speedup

**Option 2: Manual Test with Stress Test**

```bash
# Build the stress test
cmake --build build --target test_batch_validation_stress

# Test baseline (no batch validation)
unset MAKO_ENABLE_BATCH_VALIDATION
./build/test_batch_validation_stress 4 1000

# Test with batch validation
export MAKO_ENABLE_BATCH_VALIDATION=1
export MAKO_BATCH_VALIDATION_SIZE=32
./build/test_batch_validation_stress 4 1000
```

## What the Stress Test Does

The `test_batch_validation_stress` program:
- **Spawns multiple threads** (default: 4, configurable)
- **Each thread runs many transactions** (default: 1000, configurable)
- **Creates contention** (multiple threads accessing same keys)
- **Measures:**
  - Throughput (txns/sec)
  - Committed transactions
  - Aborted transactions
  - Duration

## Expected Results

**With Batch Validation:**
- ✅ Higher throughput (2-4× on multi-core)
- ✅ Better CPU utilization
- ✅ Lower latency under contention

**Without Batch Validation:**
- ❌ Lower throughput (sequential bottleneck)
- ❌ Lower CPU utilization (~25%)
- ❌ Higher latency under contention

## Command Line Options

```bash
./build/test_batch_validation_stress [num_threads] [txns_per_thread]
```

Examples:
```bash
# 4 threads, 1000 txns per thread = 4000 total txns
./build/test_batch_validation_stress 4 1000

# 8 threads, 5000 txns per thread = 40000 total txns
./build/test_batch_validation_stress 8 5000
```

## Quick Start

```bash
# 1. Build (if not already built)
cmake --build build --target test_batch_validation_stress

# 2. Run automated test
./run_performance_test.sh

# That's it! It will show:
# - Baseline throughput
# - Batch validation throughput  
# - Speedup comparison
```

## Understanding the Results

**Throughput (txns/sec):**
- Higher is better
- Batch validation should show 2-4× improvement on multi-core

**Abort Rate (%):**
- Should be similar with/without batch validation
- If much higher, there might be an issue

**Duration (seconds):**
- Batch validation might take slightly longer due to batching overhead
- But should process more transactions per second

## Troubleshooting

**If test doesn't build:**
```bash
# Rebuild everything
cmake --build build --clean-first
```

**If no performance difference:**
- Increase number of threads (8, 16)
- Increase transactions per thread (5000, 10000)
- Increase batch size: `export MAKO_BATCH_VALIDATION_SIZE=64`
- Test on a machine with more cores

**If test crashes:**
- Check logs in `/tmp/baseline_perf.log` and `/tmp/batch_perf.log`
- Reduce number of threads/transactions
- Make sure OpenMP is installed: `sudo apt-get install libomp-dev`

## Next Steps

1. Run `./run_performance_test.sh`
2. Compare throughput numbers
3. Adjust batch size and retry
4. Test with different thread counts
