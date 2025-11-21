# Quick Performance Test for Batch Validation

## ✅ Easiest Way to Test Performance

### Step 1: Run the Automated Test

```bash
./run_performance_test.sh
```

**That's it!** The script will:
- ✅ Build the stress test automatically
- ✅ Run baseline (sequential validation)
- ✅ Run with batch validation enabled
- ✅ Compare throughput and show speedup

### Step 2: Check Results

The script outputs:
- Baseline throughput (txns/sec)
- Batch validation throughput (txns/sec)
- **Speedup: X.X×** (how much faster)

## Manual Test (Alternative)

### Without Batch Validation

```bash
unset MAKO_ENABLE_BATCH_VALIDATION
./build/test_batch_validation_stress 4 1000
```

Output shows:
```
Throughput: XXX.XX txns/sec
Committed: XXXX
Aborted: XX
```

### With Batch Validation

```bash
export MAKO_ENABLE_BATCH_VALIDATION=1
export MAKO_BATCH_VALIDATION_SIZE=32
./build/test_batch_validation_stress 4 1000
```

Output shows:
```
Throughput: XXX.XX txns/sec
Committed: XXXX
Aborted: XX
```

**Compare the throughput numbers** - batch validation should be 2-4× faster!

## What the Stress Test Does

1. **Spawns multiple threads** (default: 4)
2. **Each thread runs transactions** (default: 1000 per thread)
3. **Creates contention** (threads access same keys)
4. **Measures throughput** (txns/sec)

Total: 4 threads × 1000 txns = **4000 transactions**

## Adjust Test Parameters

```bash
# More threads = more contention = bigger batch validation benefit
./build/test_batch_validation_stress 8 2000

# More transactions = better average
./build/test_batch_validation_stress 4 5000
```

## Expected Results

**On a 4-core system:**
- Baseline: ~500-1000 txns/sec
- Batch validation: ~2000-4000 txns/sec
- **Speedup: 2-4×**

**On an 8-core system:**
- Baseline: ~500-1000 txns/sec
- Batch validation: ~3000-5000 txns/sec
- **Speedup: 3-5×**

## Troubleshooting

**If test doesn't run:**
```bash
# Rebuild
cmake --build build --target test_batch_validation_stress
```

**If no speedup:**
- Increase threads: `./build/test_batch_validation_stress 8 2000`
- Increase batch size: `export MAKO_BATCH_VALIDATION_SIZE=64`
- Test on machine with more cores

**If crashes:**
- Check logs: `/tmp/baseline_perf.log` and `/tmp/batch_perf.log`
- Reduce threads/transactions
- Make sure OpenMP is installed: `sudo apt-get install libomp-dev`

## Summary

**Just run:**
```bash
./run_performance_test.sh
```

**Done!** It will show you the performance improvement from batch validation.

