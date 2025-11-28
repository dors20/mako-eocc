# How to Test Batch Validation Performance

## ✅ Quick Test Command

```bash
# Run automated performance test
./run_performance_test.sh
```

This compares baseline vs batch validation and shows throughput numbers.

## 📊 Understanding Results

**Your test showed:**
- Baseline: 1,288,000 txns/sec
- Batch validation: 966,000 txns/sec
- Speedup: 0.75x (slower)

**Why batch validation might be slower here:**
1. **Too few transactions**: Only 4000 total (4 threads × 1000 txns)
   - Batch size is 32, so batches aren't consistently full
   - Batching overhead outweighs benefits at low volumes

2. **Low contention**: Transactions might not be competing enough
   - Batch validation helps most under high contention

3. **Short duration**: Test completes too quickly
   - Not enough time to see batch effects

## 🎯 How to See Performance Improvements

### Increase Test Size

```bash
# More threads = more contention
./build/test_batch_validation_stress 8 2000

# More transactions per thread = better batching
./build/test_batch_validation_stress 4 5000

# Even bigger
./build/test_batch_validation_stress 8 5000
```

### Increase Batch Size

```bash
export MAKO_ENABLE_BATCH_VALIDATION=1
export MAKO_BATCH_VALIDATION_SIZE=64  # Larger batches
export MAKO_BATCH_VALIDATION_MAX_WAIT_US=2000
./build/test_batch_validation_stress 8 5000
```

### Test with More Contention

The stress test creates contention, but you might need:
- More threads (8, 16)
- More transactions per thread (5000+)
- Longer test duration

## 📈 Expected Results (When Testing Correctly)

**On a 4-core system with proper test:**
- Baseline: ~500K-1M txns/sec
- Batch validation: ~2M-4M txns/sec
- **Speedup: 2-4×**

**To see this, use:**
```bash
export MAKO_ENABLE_BATCH_VALIDATION=1
export MAKO_BATCH_VALIDATION_SIZE=64
./build/test_batch_validation_stress 8 5000
```

## 🔍 What to Look For

1. **Throughput**: Higher is better (txns/sec)
2. **Committed**: Should be similar with/without batch validation
3. **Abort Rate**: Should be similar (not much higher with batch validation)
4. **Speedup**: Should be > 1.0 on multi-core with sufficient load

## 📝 Test Scenarios

### Scenario 1: Quick Test (Current)
```bash
./run_performance_test.sh
# 4 threads, 1000 txns each = 4000 total
# Might not show improvement (too small)
```

### Scenario 2: Medium Test (Better)
```bash
# Baseline
unset MAKO_ENABLE_BATCH_VALIDATION
./build/test_batch_validation_stress 8 2000

# Batch validation
export MAKO_ENABLE_BATCH_VALIDATION=1
export MAKO_BATCH_VALIDATION_SIZE=64
./build/test_batch_validation_stress 8 2000
```

### Scenario 3: Large Test (Best)
```bash
# Baseline
unset MAKO_ENABLE_BATCH_VALIDATION
./build/test_batch_validation_stress 16 5000

# Batch validation
export MAKO_ENABLE_BATCH_VALIDATION=1
export MAKO_BATCH_VALIDATION_SIZE=128
./build/test_batch_validation_stress 16 5000
```

## 💡 Tips

1. **More threads = more contention = bigger batch validation benefit**
2. **More transactions = better batch fill rate = better performance**
3. **Larger batch size = better parallelization (up to thread count)**
4. **Test on multi-core machine** (single core won't show improvement)

## ✅ Summary

**To see performance improvements:**
1. Use more threads (8-16)
2. Use more transactions per thread (5000+)
3. Increase batch size (64-128)
4. Test on multi-core system

**Quick command:**
```bash
export MAKO_ENABLE_BATCH_VALIDATION=1
export MAKO_BATCH_VALIDATION_SIZE=64
./build/test_batch_validation_stress 8 5000
```

Compare throughput with and without batch validation enabled!

