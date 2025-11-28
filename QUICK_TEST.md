# Quick Test Guide for Batch Validation

## ✅ Works! Simple Test

### Test 1: With Batch Validation Enabled

```bash
# Enable batch validation
export MAKO_ENABLE_BATCH_VALIDATION=1
export MAKO_BATCH_VALIDATION_SIZE=16

# Run simple transaction test
./build/simpleTransaction
```

**Expected Output:**
```
All tests completed successfully!
```

### Test 2: With Batch Validation Disabled

```bash
# Disable batch validation
unset MAKO_ENABLE_BATCH_VALIDATION

# Run same test
./build/simpleTransaction
```

**Expected Output:**
```
All tests completed successfully!
```

### Test 3: Use the Simple Test Script

```bash
# Run the automated test script
./test_simple_batch_validation.sh
```

This will:
- Test with batch validation enabled
- Test with batch validation disabled
- Compare results

## Note on dbtest

The `dbtest` command doesn't use `-b` or `-d` flags. It requires a configuration file. To use dbtest:

1. Create or use a config file (e.g., `config/occ.yml`)
2. Run: `./build/dbtest -q config/occ.yml -t 4`

But for quick testing, `simpleTransaction` is easier and works great!

## Verify Batch Validation is Working

The feature is enabled if:
1. ✅ Built with `-DENABLE_BATCH_VALIDATION=ON` (already done)
2. ✅ Runtime env var: `export MAKO_ENABLE_BATCH_VALIDATION=1`

If both are true, transactions will use parallel batch validation automatically during commit.

## Configuration Options

```bash
# Batch size (default: 32)
export MAKO_BATCH_VALIDATION_SIZE=64

# Max wait time in microseconds (default: 1000)
export MAKO_BATCH_VALIDATION_MAX_WAIT_US=2000
```

