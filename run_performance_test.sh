#!/bin/bash

# Simple performance test for batch validation
# Creates many concurrent transactions to test batch validation

set -e

BUILD_DIR="build"
NUM_THREADS=8
TXNS_PER_THREAD=5000

echo "=========================================="
echo "Batch Validation Performance Test"
echo "=========================================="
echo ""

# Step 1: Build the stress test
echo "Step 1: Building stress test..."
if [ ! -f "$BUILD_DIR/test_batch_validation_stress" ]; then
    echo "Building test_batch_validation_stress..."
    cmake --build $BUILD_DIR --target test_batch_validation_stress
    if [ ! -f "$BUILD_DIR/test_batch_validation_stress" ]; then
        echo "❌ ERROR: Failed to build stress test"
        exit 1
    fi
fi
echo "✅ Stress test built: $BUILD_DIR/test_batch_validation_stress"
echo ""

# Step 2: Test with batch validation disabled
echo "=========================================="
echo "Test 1: Baseline (Sequential Validation)"
echo "=========================================="
unset MAKO_ENABLE_BATCH_VALIDATION
echo "Configuration:"
echo "  MAKO_ENABLE_BATCH_VALIDATION: ${MAKO_ENABLE_BATCH_VALIDATION:-disabled}"
echo "  Threads: $NUM_THREADS"
echo "  Transactions per thread: $TXNS_PER_THREAD"
echo ""

echo "Running stress test..."
./$BUILD_DIR/test_batch_validation_stress $NUM_THREADS $TXNS_PER_THREAD > /tmp/baseline_perf.log 2>&1
echo "Results saved to /tmp/baseline_perf.log"
echo ""

# Extract baseline throughput (match the full number before " txns/sec")
BASELINE_TPS=$(grep "Throughput:" /tmp/baseline_perf.log | sed -n 's/.*Throughput:[[:space:]]*\([0-9]\+\)[[:space:]]*txns\/sec.*/\1/p' | head -1 || echo "N/A")
BASELINE_COMMITTED=$(grep "^Committed:" /tmp/baseline_perf.log | sed -n 's/.*Committed:[[:space:]]*\([0-9]\+\).*/\1/p' | head -1 || echo "N/A")
echo "Baseline:"
echo "  Throughput: $BASELINE_TPS txns/sec"
echo "  Committed: $BASELINE_COMMITTED txns"
echo ""

# Step 3: Test with batch validation enabled
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
echo "  Threads: $NUM_THREADS"
echo "  Transactions per thread: $TXNS_PER_THREAD"
echo ""

echo "Running stress test..."
./$BUILD_DIR/test_batch_validation_stress $NUM_THREADS $TXNS_PER_THREAD > /tmp/batch_perf.log 2>&1
echo "Results saved to /tmp/batch_perf.log"
echo ""

# Extract batch validation throughput (match the full number before " txns/sec")
BATCH_TPS=$(grep "Throughput:" /tmp/batch_perf.log | sed -n 's/.*Throughput:[[:space:]]*\([0-9]\+\)[[:space:]]*txns\/sec.*/\1/p' | head -1 || echo "N/A")
BATCH_COMMITTED=$(grep "^Committed:" /tmp/batch_perf.log | sed -n 's/.*Committed:[[:space:]]*\([0-9]\+\).*/\1/p' | head -1 || echo "N/A")
echo "Batch Validation:"
echo "  Throughput: $BATCH_TPS txns/sec"
echo "  Committed: $BATCH_COMMITTED txns"
echo ""

# Step 4: Compare results
echo "=========================================="
echo "Performance Comparison"
echo "=========================================="
echo "Baseline Throughput:     $BASELINE_TPS txns/sec"
echo "Batch Validation:        $BATCH_TPS txns/sec"
echo ""

if [ "$BASELINE_TPS" != "N/A" ] && [ "$BATCH_TPS" != "N/A" ]; then
    if command -v bc &> /dev/null && [ "$BASELINE_TPS" -gt 0 ] 2>/dev/null; then
        SPEEDUP=$(echo "scale=2; $BATCH_TPS / $BASELINE_TPS" | bc 2>/dev/null || echo "1.0")
        echo "Speedup: ${SPEEDUP}x"
        
        COMPARE=$(echo "$SPEEDUP > 1.1" | bc 2>/dev/null || echo "0")
        if [ "$COMPARE" = "1" ]; then
            echo "✅ Batch validation improved performance!"
        elif [ "$(echo "$SPEEDUP < 0.9" | bc 2>/dev/null || echo "0")" = "1" ]; then
            echo "⚠️  Batch validation slightly slower (might be overhead)"
        else
            echo "⚠️  No significant performance difference"
        fi
    else
        echo "Note: Could not calculate speedup (bc not available or invalid numbers)"
    fi
fi

echo ""
echo "Detailed results:"
echo "  - Baseline: /tmp/baseline_perf.log"
echo "  - Batch validation: /tmp/batch_perf.log"
echo ""

