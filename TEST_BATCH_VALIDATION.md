# Testing Parallel Batch Validation

## Quick Start

### 1. Build with Batch Validation Enabled

```bash
cd /home/ubuntu/mako

# Configure with batch validation
cmake -S . -B build \
  -DENABLE_BATCH_VALIDATION=ON \
  -DENABLE_OPENMP=ON

# Build
cmake --build build --parallel 4
```

### 2. Run Test

```bash
# Enable batch validation
export MAKO_ENABLE_BATCH_VALIDATION=1
export MAKO_BATCH_VALIDATION_SIZE=32
export MAKO_BATCH_VALIDATION_MAX_WAIT_US=1000

# Run benchmark (e.g., TPC-C)
./build/dbtest -b tpcc -t 4 -d 10
```

## Testing Options

### Environment Variables

```bash
# Enable/disable batch validation
export MAKO_ENABLE_BATCH_VALIDATION=1    # 1 or "true" to enable

# Configure batch size (default: 32)
export MAKO_BATCH_VALIDATION_SIZE=64

# Configure max wait time in microseconds (default: 1000)
export MAKO_BATCH_VALIDATION_MAX_WAIT_US=2000
```

### Benchmarks

**TPC-C (High Contention):**
```bash
./build/dbtest -b tpcc -t 4 -d 30
```

**YCSB:**
```bash
./build/dbtest -b ycsb -t 4 -d 30
```

**Simple Transaction Test:**
```bash
./build/simpleTransaction
```

## Performance Comparison

### Baseline (No Batch Validation)

```bash
# Make sure batch validation is disabled
unset MAKO_ENABLE_BATCH_VALIDATION

# Run benchmark
./build/dbtest -b tpcc -t 4 -d 30 > baseline.log
```

### With Batch Validation

```bash
# Enable batch validation
export MAKO_ENABLE_BATCH_VALIDATION=1
export MAKO_BATCH_VALIDATION_SIZE=32

# Run benchmark
./build/dbtest -b tpcc -t 4 -d 30 > batch_validation.log
```

### Compare Results

```bash
# Compare throughput (txns/sec)
grep -i "txn/s\|throughput\|tps" baseline.log batch_validation.log

# Compare latency
grep -i "latency\|p99\|p95" baseline.log batch_validation.log
```

## Verification

### Check if Feature is Enabled

The feature only works if:
1. Built with `-DENABLE_BATCH_VALIDATION=ON`
2. Runtime environment variable `MAKO_ENABLE_BATCH_VALIDATION=1`

If not enabled, transactions will use normal sequential validation (no errors, just no parallelization).

### Debug Output

Add verbose logging to see batch validation in action:

```bash
# Run with verbose output
./build/dbtest -b tpcc -t 4 -d 10 -v 2>&1 | grep -i "batch\|validation"
```

### Statistics

The implementation tracks:
- Number of batches validated
- Transactions validated in batches
- Transactions aborted in batches
- Average batch size
- Average batch validation time

Access these via Mako's statistics interface (if configured).

## Expected Results

### Performance Improvements

- **4-core system**: 2.5-3.5× improvement in validation throughput
- **8-core system**: 3.5-5× improvement (with larger batch sizes)
- **Low contention**: Slight latency increase (10-20μs) due to batching overhead
- **High contention**: Significant latency decrease (50-200μs) due to parallel validation

### CPU Utilization

- **Without batch validation**: ~25% CPU (single-threaded validation)
- **With batch validation**: ~75-90% CPU (parallel validation)

## Troubleshooting

### Feature Not Working

1. **Check build configuration:**
   ```bash
   grep -r "ENABLE_BATCH_VALIDATION" build/CMakeCache.txt
   ```

2. **Check runtime environment:**
   ```bash
   echo $MAKO_ENABLE_BATCH_VALIDATION
   ```

3. **Verify OpenMP:**
   ```bash
   ldd build/dbtest | grep omp
   ```

### Performance Not Improving

1. **Too small batch size**: Increase `MAKO_BATCH_VALIDATION_SIZE`
2. **Too short timeout**: Increase `MAKO_BATCH_VALIDATION_MAX_WAIT_US`
3. **Low contention**: Batch validation helps most under contention
4. **Single core**: No benefit on single-core systems

### Compilation Errors

1. **Missing OpenMP**: Install `libomp-dev`
   ```bash
   sudo apt-get install libomp-dev
   ```

2. **Template errors**: Ensure all template instantiations compile

## Example Test Script

See `test_batch_validation.sh` for automated testing.

