# Parallel Batch Validation for Mako

## Overview

This implementation adds **parallel batch validation** to Mako's transaction commit path. Instead of validating transactions one-by-one sequentially, transactions are batched together and validated in parallel across multiple threads, improving throughput under contention.

## How It Works

1. **Batching**: Transactions ready to commit are collected into batches (default: 32 transactions)
2. **Parallel Validation**: When a batch is full or a timeout is reached, all transactions in the batch are validated in parallel using OpenMP or thread-based parallel execution
3. **Result Processing**: After parallel validation completes, transactions that pass validation proceed to the write phase; those that fail are aborted

## Key Components

### `src/mako/txn_occ_batch_validation.h`

The `BatchValidator` class implements:
- **Batch Collection**: Collects transactions ready to commit
- **Parallel Validation**: Validates multiple transactions simultaneously using OpenMP
- **Synchronization**: Ensures transactions wait for batch validation results before proceeding

### Integration in `src/mako/txn_impl.h`

The commit path (`transaction::commit()`) has been modified to:
- Check if batch validation is enabled
- Add transactions to batches for parallel validation
- Skip individual validation if validation already completed in batch

## Usage

### Enable Batch Validation

Set environment variable:
```bash
export MAKO_ENABLE_BATCH_VALIDATION=1
```

### Configuration Options

```bash
# Batch size (default: 32)
export MAKO_BATCH_VALIDATION_SIZE=64

# Maximum wait time in microseconds before validating incomplete batch (default: 1000)
export MAKO_BATCH_VALIDATION_MAX_WAIT_US=2000
```

### Build with OpenMP Support

For optimal performance, compile with OpenMP:
```bash
make CXXFLAGS="-DENABLE_BATCH_VALIDATION -fopenmp"
```

Or add to your build configuration:
```cmake
find_package(OpenMP)
target_link_libraries(your_target OpenMP::OpenMP_CXX)
```

## Architecture

### Current Validation Flow (Sequential)

```
Transaction 1 → Validate → Write
Transaction 2 → Validate → Write
Transaction 3 → Validate → Write
```

### Parallel Batch Validation Flow

```
Transaction 1 ┐
Transaction 2 ├─→ Batch → Parallel Validate (4 threads) → Results → Write
Transaction 3 │
Transaction 4 ┘
```

## Implementation Details

### Validation Logic

The batch validator validates transactions using the same logic as individual validation:
- **Read Set Validation**: Checks if read tuples are still the latest version
- **Absent Set Validation**: Checks if btree versions have changed
- **Write Set Conflict Detection**: Ensures no write-write conflicts

### Thread Safety

- Uses mutex and condition variables for batch synchronization
- Thread-safe atomic operations for validation counters
- Each validation thread operates on independent transaction chunks

### Performance Characteristics

- **Reduced Validation Latency**: Parallel validation reduces wall-clock time for batch validation
- **Better CPU Utilization**: Multiple cores can validate transactions simultaneously
- **Trade-off**: Slight increase in commit latency due to batching, but higher throughput

## Statistics

The batch validator tracks:
- `g_evt_batch_validations`: Number of batches validated
- `g_evt_batch_validated_txns`: Number of transactions that passed batch validation
- `g_evt_batch_aborted_txns`: Number of transactions aborted during batch validation
- `g_evt_avg_batch_size`: Average batch size
- `g_evt_avg_batch_validation_time_us`: Average batch validation time in microseconds

## Limitations and Future Work

### Current Limitations

1. **Synchronous Batching**: Transactions block waiting for batch to fill/timeout
2. **OpenMP Dependency**: Optimal performance requires OpenMP support
3. **Memory Overhead**: Batch structures allocate memory for pending transactions

### Future Enhancements

1. **Asynchronous Validation**: Non-blocking batch collection and validation
2. **Adaptive Batching**: Dynamically adjust batch size based on workload
3. **NUMA-Aware Validation**: Assign validation threads to specific NUMA nodes
4. **Validation Optimization**: Parallel read set checking across transactions

## Testing

To test parallel batch validation:

```bash
# Enable batch validation
export MAKO_ENABLE_BATCH_VALIDATION=1
export MAKO_BATCH_VALIDATION_SIZE=32

# Run benchmark
./build/dbtest -b tpcc -t 4
```

Compare throughput and latency with and without batch validation enabled.

## Code Locations

- **Header**: `src/mako/txn_occ_batch_validation.h`
- **Integration**: `src/mako/txn_impl.h` (lines 335-434)
- **Friend Declaration**: `src/mako/txn.h` (line 418)


