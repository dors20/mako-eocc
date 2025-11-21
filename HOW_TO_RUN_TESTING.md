# How to Run Testing for Batch Validation

## Table of Contents
1. [Quick Start](#quick-start)
2. [Testing Methods](#testing-methods)
3. [Test Scripts Available](#test-scripts-available)
4. [Manual Testing](#manual-testing)
5. [Environment Variables](#environment-variables)
6. [Interpreting Results](#interpreting-results)
7. [Troubleshooting](#troubleshooting)

---

## Quick Start

### Option 1: Automated Performance Test (Recommended)
```bash
cd /home/ubuntu/mako
./run_performance_test.sh
```

This will:
- Build the stress test if needed
- Run baseline test (sequential validation)
- Run with batch validation enabled
- Compare results and show speedup

### Option 2: Quick Functional Test
```bash
cd /home/ubuntu/mako
./test_simple_batch_validation.sh
```

This verifies that batch validation compiles and runs without errors.

---

## Testing Methods

### 1. Functional Testing (Correctness)
**Purpose:** Verify batch validation works correctly without crashing.

**Script:**
```bash
./test_simple_batch_validation.sh
```

**Manual Test:**
```bash
# Build if needed
cmake --build build --target simpleTransaction

# Test with batch validation enabled
export MAKO_ENABLE_BATCH_VALIDATION=1
export MAKO_BATCH_VALIDATION_SIZE=16
timeout 10s ./build/simpleTransaction

# Test with batch validation disabled
unset MAKO_ENABLE_BATCH_VALIDATION
timeout 10s ./build/simpleTransaction
```

**What it checks:**
- ✅ Compiles successfully
- ✅ Runs without crashing
- ✅ Basic CRUD operations work
- ❌ Does NOT measure performance
- ❌ Does NOT test high contention

---

### 2. Performance Testing (Throughput)
**Purpose:** Measure throughput improvement with batch validation.

**Script:**
```bash
./run_performance_test.sh
```

**Manual Test:**
```bash
# Build stress test
cmake --build build --target test_batch_validation_stress

# Baseline (sequential validation)
unset MAKO_ENABLE_BATCH_VALIDATION
./build/test_batch_validation_stress 4 1000

# With batch validation
export MAKO_ENABLE_BATCH_VALIDATION=1
export MAKO_BATCH_VALIDATION_SIZE=32
export MAKO_BATCH_VALIDATION_MAX_WAIT_US=1000
./build/test_batch_validation_stress 4 1000
```

**Output includes:**
- Throughput (txns/sec)
- Committed transactions
- Aborted transactions
- Duration
- Abort rate

---

### 3. Stress Testing (High Load)
**Purpose:** Test batch validation under high contention.

```bash
# Build stress test
cmake --build build --target test_batch_validation_stress

# High contention test
export MAKO_ENABLE_BATCH_VALIDATION=1
export MAKO_BATCH_VALIDATION_SIZE=64
export MAKO_BATCH_VALIDATION_MAX_WAIT_US=5000

# 8 threads, 5000 txns each = 40,000 total
./build/test_batch_validation_stress 8 5000
```

---

## Test Scripts Available

### 1. `run_performance_test.sh`
**Purpose:** Automated performance comparison  
**Usage:**
```bash
./run_performance_test.sh
```
**What it does:**
- Builds `test_batch_validation_stress` if needed
- Runs baseline test (sequential)
- Runs with batch validation
- Compares throughput and calculates speedup
- Saves logs to `/tmp/baseline_perf.log` and `/tmp/batch_perf.log`

### 2. `test_simple_batch_validation.sh`
**Purpose:** Quick functional correctness test  
**Usage:**
```bash
./test_simple_batch_validation.sh
```
**What it does:**
- Builds `simpleTransaction` if needed
- Runs with batch validation enabled
- Runs with batch validation disabled
- Verifies no crashes

### 3. `test_batch_validation_performance.sh`
**Purpose:** Alternative performance test script  
**Usage:**
```bash
./test_batch_validation_performance.sh
```

---

## Manual Testing

### Build Requirements

First, ensure batch validation is enabled in the build:
```bash
cd /home/ubuntu/mako

# Configure build with batch validation and OpenMP
cmake -S . -B build \
  -DENABLE_BATCH_VALIDATION=ON \
  -DENABLE_OPENMP=ON

# Build
cmake --build build --parallel 4
```

### Test Executables

#### 1. `simpleTransaction`
**Purpose:** Basic functional test  
**What it tests:**
- Basic CRUD operations
- Transaction correctness
- No performance measurement

**Usage:**
```bash
export MAKO_ENABLE_BATCH_VALIDATION=1
./build/simpleTransaction
```

#### 2. `test_batch_validation_stress`
**Purpose:** Performance and stress testing  
**What it tests:**
- Throughput under contention
- Multiple threads
- Many transactions
- Abort rates

**Usage:**
```bash
./build/test_batch_validation_stress [num_threads] [txns_per_thread]
```

**Examples:**
```bash
# Small test: 4 threads, 1000 txns each
./build/test_batch_validation_stress 4 1000

# Medium test: 8 threads, 5000 txns each
./build/test_batch_validation_stress 8 5000

# Large test: 16 threads, 10000 txns each
./build/test_batch_validation_stress 16 10000
```

---

## Environment Variables

### Required Variables

#### `MAKO_ENABLE_BATCH_VALIDATION`
**Purpose:** Enable/disable batch validation  
**Values:**
- `1` or `true` - Enable batch validation
- `0` or `false` - Disable batch validation
- Unset - Disable batch validation

**Example:**
```bash
export MAKO_ENABLE_BATCH_VALIDATION=1
```

### Optional Variables

#### `MAKO_BATCH_VALIDATION_SIZE`
**Purpose:** Number of transactions per batch  
**Default:** `32`  
**Recommended:** `32-128`  
**Example:**
```bash
export MAKO_BATCH_VALIDATION_SIZE=64
```

**Guidelines:**
- **Small batches (16-32):** Lower latency, less batching overhead
- **Medium batches (32-64):** Balanced performance
- **Large batches (64-128):** Higher throughput, more batching overhead

#### `MAKO_BATCH_VALIDATION_MAX_WAIT_US`
**Purpose:** Maximum time to wait for batch to fill (microseconds)  
**Default:** `1000` (1ms)  
**Recommended:** `1000-10000`  
**Example:**
```bash
export MAKO_BATCH_VALIDATION_MAX_WAIT_US=5000
```

**Guidelines:**
- **Short wait (500-1000μs):** Lower latency, smaller batches
- **Medium wait (1000-5000μs):** Balanced
- **Long wait (5000-10000μs):** Larger batches, higher latency

### Setting Environment Variables

**For single test:**
```bash
export MAKO_ENABLE_BATCH_VALIDATION=1
export MAKO_BATCH_VALIDATION_SIZE=64
export MAKO_BATCH_VALIDATION_MAX_WAIT_US=1000
./build/test_batch_validation_stress 8 5000
```

**Disable for baseline:**
```bash
unset MAKO_ENABLE_BATCH_VALIDATION
./build/test_batch_validation_stress 8 5000
```

---

## Interpreting Results

### Test Output Format

```
==========================================
Results
==========================================
Duration: 14 ms (0.014 s)
Committed: 39860
Aborted: 140
Total: 40000
Abort Rate: 0.35%
Throughput: 2847143 txns/sec
```

### Key Metrics

#### Throughput (txns/sec)
**What it means:** Transactions processed per second  
**Good value:** Higher is better  
**Expected improvement:** 2-4× with batch validation on multi-core

**Interpretation:**
- ✅ **Higher with batch validation:** Working correctly
- ⚠️ **Similar with/without:** Test too small or low contention
- ❌ **Lower with batch validation:** Overhead outweighing benefits (small test)

#### Committed Transactions
**What it means:** Successful transactions  
**Good value:** Should be similar with/without batch validation  
**Expected:** Slight difference is normal (abort rate may vary)

#### Aborted Transactions
**What it means:** Failed validations  
**Expected:** Similar abort rate with/without batch validation  
**Warning:** If much higher with batch validation, there may be an issue

#### Abort Rate (%)
**What it means:** Percentage of transactions that aborted  
**Expected:** Similar with/without batch validation (< 5% typically)

#### Duration
**What it means:** Time to complete all transactions  
**Note:** May be longer with batch validation due to batching, but throughput should be higher

### Comparing Results

**Example comparison:**
```
Baseline (Sequential):
  Throughput: 3,066,462 txns/sec
  Duration: 13ms
  Committed: 39,864

Batch Validation:
  Throughput: 2,847,143 txns/sec
  Duration: 14ms
  Committed: 39,860

Speedup: 0.93× (slower)
```

**What this means:**
- Test is too small/fast to show benefits
- Batching overhead > parallelization benefit at this scale
- Need larger test with more contention

**To see improvements:**
- Increase threads: `./build/test_batch_validation_stress 16 5000`
- Increase transactions: `./build/test_batch_validation_stress 8 10000`
- Increase batch size: `export MAKO_BATCH_VALIDATION_SIZE=128`

---

## Troubleshooting

### Build Issues

**Problem:** Test doesn't build  
**Solution:**
```bash
# Clean and rebuild
cmake --build build --clean-first --target test_batch_validation_stress
```

**Problem:** OpenMP not found  
**Solution:**
```bash
# Install OpenMP
sudo apt-get install libomp-dev

# Reconfigure build
cmake -S . -B build -DENABLE_OPENMP=ON
cmake --build build
```

### Runtime Issues

**Problem:** Segmentation fault  
**Solution:**
- Reduce thread count: `./build/test_batch_validation_stress 2 1000`
- Check logs in `/tmp/baseline_perf.log`
- Verify OpenMP is installed

**Problem:** Test hangs  
**Solution:**
- Add timeout: `timeout 30s ./build/test_batch_validation_stress 8 5000`
- Check if batch validation is causing deadlock
- Reduce batch size: `export MAKO_BATCH_VALIDATION_SIZE=16`

### Performance Issues

**Problem:** No performance improvement  
**Solutions:**
1. **Increase test size:**
   ```bash
   ./build/test_batch_validation_stress 16 10000
   ```

2. **Increase batch size:**
   ```bash
   export MAKO_BATCH_VALIDATION_SIZE=128
   ```

3. **Increase wait time:**
   ```bash
   export MAKO_BATCH_VALIDATION_MAX_WAIT_US=10000
   ```

4. **Check CPU cores:**
   ```bash
   nproc  # Should have multiple cores
   ```

5. **Verify OpenMP is enabled:**
   ```bash
   cmake --build build 2>&1 | grep -i "openmp"
   ```

**Problem:** Performance is worse  
**Expected for small tests:** Batching overhead can outweigh benefits  
**Solutions:**
- Test with larger workloads (more threads, more transactions)
- Reduce batch size: `export MAKO_BATCH_VALIDATION_SIZE=16`
- Reduce wait time: `export MAKO_BATCH_VALIDATION_MAX_WAIT_US=500`

### Verification

**Verify batch validation is enabled:**
```bash
export MAKO_ENABLE_BATCH_VALIDATION=1
./build/test_batch_validation_stress 2 100 | grep "Batch validation"
```

Should show: `✓ Batch validation: ENABLED`

**Verify OpenMP is working:**
```bash
export OMP_NUM_THREADS=4
export MAKO_ENABLE_BATCH_VALIDATION=1
./build/test_batch_validation_stress 8 5000
```

---

## Recommended Testing Workflow

### 1. Quick Verification
```bash
./test_simple_batch_validation.sh
```
Verify it compiles and runs.

### 2. Performance Test
```bash
./run_performance_test.sh
```
Get initial performance numbers.

### 3. Larger Test (if needed)
```bash
export MAKO_ENABLE_BATCH_VALIDATION=1
export MAKO_BATCH_VALIDATION_SIZE=64
./build/test_batch_validation_stress 8 5000
```

### 4. Compare Multiple Configurations
```bash
# Baseline
unset MAKO_ENABLE_BATCH_VALIDATION
./build/test_batch_validation_stress 8 5000 > baseline.log

# Small batch
export MAKO_ENABLE_BATCH_VALIDATION=1
export MAKO_BATCH_VALIDATION_SIZE=16
./build/test_batch_validation_stress 8 5000 > batch16.log

# Large batch
export MAKO_BATCH_VALIDATION_SIZE=128
./build/test_batch_validation_stress 8 5000 > batch128.log

# Compare results
grep "Throughput:" baseline.log batch16.log batch128.log
```

---

## Summary

### Quick Commands

**Automated test:**
```bash
./run_performance_test.sh
```

**Manual test:**
```bash
# Build
cmake --build build --target test_batch_validation_stress

# Baseline
unset MAKO_ENABLE_BATCH_VALIDATION
./build/test_batch_validation_stress 4 1000

# With batch validation
export MAKO_ENABLE_BATCH_VALIDATION=1
export MAKO_BATCH_VALIDATION_SIZE=32
./build/test_batch_validation_stress 4 1000
```

**Check if working:**
```bash
export MAKO_ENABLE_BATCH_VALIDATION=1
./build/test_batch_validation_stress 2 100 | grep "Batch validation"
```

---

For more details, see:
- `HOW_TO_TEST_PERFORMANCE.md` - Detailed performance testing guide
- `PERFORMANCE_ANALYSIS.md` - Understanding performance results
- `PARALLEL_VALIDATION.md` - Feature documentation



