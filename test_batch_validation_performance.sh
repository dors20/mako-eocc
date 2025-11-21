#!/bin/bash

# Performance test for parallel batch validation
# Compares throughput with and without batch validation

set -e

BUILD_DIR="build"
TEST_DURATION=30
NUM_THREADS=4

echo "=========================================="
echo "Batch Validation Performance Test"
echo "=========================================="
echo ""

# Check if build exists
if [ ! -f "$BUILD_DIR/simpleTransaction" ]; then
    echo "ERROR: simpleTransaction not found. Please build first."
    exit 1
fi

echo "Test Configuration:"
echo "  Duration: $TEST_DURATION seconds"
echo "  Threads: $NUM_THREADS"
echo "  Build: $BUILD_DIR"
echo ""

# Create a simple performance test using config file
# Since dbtest needs config files, let's create a minimal test

# Test 1: Baseline (no batch validation)
echo "=========================================="
echo "Test 1: Baseline (Sequential Validation)"
echo "=========================================="
unset MAKO_ENABLE_BATCH_VALIDATION
unset MAKO_BATCH_VALIDATION_SIZE
unset MAKO_BATCH_VALIDATION_MAX_WAIT_US

echo "Configuration:"
echo "  MAKO_ENABLE_BATCH_VALIDATION: ${MAKO_ENABLE_BATCH_VALIDATION:-disabled}"
echo ""

echo "Running simpleTransaction (baseline)..."
START_TIME=$(date +%s.%N)
for i in $(seq 1 10); do
    ./$BUILD_DIR/simpleTransaction > /tmp/baseline_run_$i.log 2>&1 || true
done
END_TIME=$(date +%s.%N)
BASELINE_TIME=$(echo "$END_TIME - $START_TIME" | bc)
echo "Baseline: Completed 10 runs in ${BASELINE_TIME}s"
echo ""

# Test 2: With batch validation
echo "=========================================="
echo "Test 2: With Batch Validation"
echo "=========================================="
export MAKO_ENABLE_BATCH_VALIDATION=1
export MAKO_BATCH_VALIDATION_SIZE=32
export MAKO_BATCH_VALIDATION_MAX_WAIT_US=1000

echo "Configuration:"
echo "  MAKO_ENABLE_BATCH_VALIDATION: $MAKO_ENABLE_BATCH_VALIDATION"
echo "  MAKO_BATCH_VALIDATION_SIZE: $MAKO_BATCH_VALIDATION_SIZE"
echo "  MAKO_BATCH_VALIDATION_MAX_WAIT_US: $MAKO_BATCH_VALIDATION_MAX_WAIT_US"
echo ""

echo "Running simpleTransaction (with batch validation)..."
START_TIME=$(date +%s.%N)
for i in $(seq 1 10); do
    ./$BUILD_DIR/simpleTransaction > /tmp/batch_run_$i.log 2>&1 || true
done
END_TIME=$(date +%s.%N)
BATCH_TIME=$(echo "$END_TIME - $START_TIME" | bc)
echo "Batch Validation: Completed 10 runs in ${BATCH_TIME}s"
echo ""

# Summary
echo "=========================================="
echo "Performance Summary"
echo "=========================================="
echo "Baseline Time:        ${BASELINE_TIME}s"
echo "Batch Validation Time: ${BATCH_TIME}s"
echo ""

if command -v bc &> /dev/null; then
    if (( $(echo "$BASELINE_TIME > 0" | bc -l) )); then
        SPEEDUP=$(echo "scale=2; $BASELINE_TIME / $BATCH_TIME" | bc)
        echo "Speedup: ${SPEEDUP}x"
    fi
fi

echo ""
echo "Note: This is a simple test. For real performance testing,"
echo "you need many concurrent transactions with contention."
echo "See HOW_TO_TEST_PERFORMANCE.md for detailed instructions."

