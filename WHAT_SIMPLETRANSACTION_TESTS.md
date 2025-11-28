# What Does `simpleTransaction` Test?

## Overview

`simpleTransaction` is a **functional correctness test** for Mako's transaction system. It tests basic transaction operations to ensure they work correctly.

## What It Tests

### Test 1: Basic Transactions

1. **Write 5 Records** (lines 32-48)
   - Creates 5 transactions
   - Each transaction writes one key-value pair:
     - Keys: `test_key_0`, `test_key_1`, ..., `test_key_4`
     - Values: `test_value_0`, `test_value_1`, ..., `test_value_4`
   - Commits each transaction
   - ✅ Verifies: All writes succeed

2. **Read 5 Records** (lines 50-72)
   - Creates 5 transactions
   - Each transaction reads one key
   - Commits each transaction
   - ✅ Verifies: Reads return correct values

3. **Table Scan** (lines 74-87)
   - Scans the entire table
   - ✅ Verifies: All 5 records are present and correct

### Test 2: Overwritten Operations

1. **Write Initial Value** (lines 96-108)
   - Writes key `overwrite_key` with value `initial_2000`
   - Commits transaction

2. **Overwrite Twice** (lines 110-137)
   - Overwrites `overwrite_key` with `updated_1000`
   - Then overwrites again with `updated_0000`
   - Both commits succeed

3. **Read Final Value** (lines 139-152)
   - Reads `overwrite_key`
   - ✅ Verifies: Value is `updated_0000` (the last write)

## What This Tests

✅ **Functional Correctness:**
- Transactions can write data
- Transactions can read data
- Transactions can overwrite data
- Table scans work correctly
- Commit/abort handling works

✅ **Transaction Path:**
- Each `db->commit_txn(txn)` calls the commit path
- If batch validation is enabled, transactions will be batched and validated in parallel
- The test verifies correctness, not performance

## Does It Test Batch Validation?

**Indirectly, yes!** 

- Every `commit_txn()` goes through the validation path
- With `MAKO_ENABLE_BATCH_VALIDATION=1`, those commits will use parallel batch validation
- The test verifies that transactions still work correctly with batch validation enabled

**However**, this is **NOT** a performance test:
- It only does 5 writes, 5 reads, and a few overwrites
- Too few transactions to see batching effects (batch size is 32 by default)
- It's a correctness test, not a throughput benchmark

## For Performance Testing

To actually test batch validation performance, you'd need:
- **More transactions** (hundreds or thousands)
- **Higher contention** (many threads competing)
- **Longer duration** (to see throughput differences)
- **Measurements** (txns/sec, latency)

The test confirms batch validation doesn't break correctness, but doesn't measure performance improvements.

