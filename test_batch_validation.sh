#!/bin/bash

# Test script for parallel batch validation
# Compares performance with and without batch validation

set -e

BUILD_DIR="build"
TEST_DURATION=30
NUM_THREADS=4
BENCHMARK="tpcc"

echo "=========================================="
echo "Parallel Batch Validation Test"
echo "=========================================="
echo ""

# Check if build exists
if [ ! -f "$BUILD_DIR/dbtest" ]; then
    echo "ERROR: dbtest not found. Please build first:"
    echo "  cmake -S . -B build -DENABLE_BATCH_VALIDATION=ON -DENABLE_OPENMP=ON"
    echo "  cmake --build build"
    exit 1
fi

echo "Build found: $BUILD_DIR/dbtest"
echo ""

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

echo "Running benchmark..."
echo "Command: ./$BUILD_DIR/dbtest -b $BENCHMARK -t $NUM_THREADS -d $TEST_DURATION"
./$BUILD_DIR/dbtest -b $BENCHMARK -t $NUM_THREADS -d $TEST_DURATION > baseline_output.log 2>&1 || true

echo "Baseline test completed. Output saved to baseline_output.log"
echo ""

# Extract metrics from baseline
BASELINE_TPS=$(grep -i "tps\|txn/s\|throughput" baseline_output.log | grep -oE "[0-9]+\.[0-9]+" | head -1 || echo "N/A")
echo "Baseline Throughput: $BASELINE_TPS txns/sec"
echo ""

# Test 2: With batch validation (default settings)
echo "=========================================="
echo "Test 2: With Batch Validation (Default)"
echo "=========================================="
export MAKO_ENABLE_BATCH_VALIDATION=1
export MAKO_BATCH_VALIDATION_SIZE=32
export MAKO_BATCH_VALIDATION_MAX_WAIT_US=1000

echo "Configuration:"
echo "  MAKO_ENABLE_BATCH_VALIDATION: $MAKO_ENABLE_BATCH_VALIDATION"
echo "  MAKO_BATCH_VALIDATION_SIZE: $MAKO_BATCH_VALIDATION_SIZE"
echo "  MAKO_BATCH_VALIDATION_MAX_WAIT_US: $MAKO_BATCH_VALIDATION_MAX_WAIT_US"
echo ""

echo "Running benchmark..."
echo "Command: ./$BUILD_DIR/dbtest -b $BENCHMARK -t $NUM_THREADS -d $TEST_DURATION"
./$BUILD_DIR/dbtest -b $BENCHMARK -t $NUM_THREADS -d $TEST_DURATION > batch_validation_output.log 2>&1 || true

echo "Batch validation test completed. Output saved to batch_validation_output.log"
echo ""

# Extract metrics from batch validation run
BATCH_TPS=$(grep -i "tps\|txn/s\|throughput" batch_validation_output.log | grep -oE "[0-9]+\.[0-9]+" | head -1 || echo "N/A")
echo "Batch Validation Throughput: $BATCH_TPS txns/sec"
echo ""

# Test 3: With batch validation (larger batch size)
echo "=========================================="
echo "Test 3: With Batch Validation (Large Batch)"
echo "=========================================="
export MAKO_ENABLE_BATCH_VALIDATION=1
export MAKO_BATCH_VALIDATION_SIZE=64
export MAKO_BATCH_VALIDATION_MAX_WAIT_US=2000

echo "Configuration:"
echo "  MAKO_ENABLE_BATCH_VALIDATION: $MAKO_ENABLE_BATCH_VALIDATION"
echo "  MAKO_BATCH_VALIDATION_SIZE: $MAKO_BATCH_VALIDATION_SIZE"
echo "  MAKO_BATCH_VALIDATION_MAX_WAIT_US: $MAKO_BATCH_VALIDATION_MAX_WAIT_US"
echo ""

echo "Running benchmark..."
echo "Command: ./$BUILD_DIR/dbtest -b $BENCHMARK -t $NUM_THREADS -d $TEST_DURATION"
./$BUILD_DIR/dbtest -b $BENCHMARK -t $NUM_THREADS -d $TEST_DURATION > batch_validation_large_output.log 2>&1 || true

echo "Large batch test completed. Output saved to batch_validation_large_output.log"
echo ""

# Extract metrics
LARGE_BATCH_TPS=$(grep -i "tps\|txn/s\|throughput" batch_validation_large_output.log | grep -oE "[0-9]+\.[0-9]+" | head -1 || echo "N/A")
echo "Large Batch Throughput: $LARGE_BATCH_TPS txns/sec"
echo ""

# Summary
echo "=========================================="
echo "Test Summary"
echo "=========================================="
echo "Baseline (Sequential):        $BASELINE_TPS txns/sec"
echo "Batch Validation (Size=32):   $BATCH_TPS txns/sec"
echo "Batch Validation (Size=64):   $LARGE_BATCH_TPS txns/sec"
echo ""

# Cleanup
echo "Output files:"
echo "  - baseline_output.log"
echo "  - batch_validation_output.log"
echo "  - batch_validation_large_output.log"
echo ""

# Calculate improvement (if we got numbers)
if [ "$BASELINE_TPS" != "N/A" ] && [ "$BATCH_TPS" != "N/A" ]; then
    if command -v bc &> /dev/null; then
        SPEEDUP=$(echo "scale=2; $BATCH_TPS / $BASELINE_TPS" | bc)
        echo "Speedup: ${SPEEDUP}x"
    fi
fi

echo ""
echo "Test completed!"

