#!/bin/bash

# Script to run OCC analysis with varying contention levels
# Usage: ./scripts/run_occ_analysis.sh [config_file]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# Configuration
OUTPUT_DIR="${PROJECT_ROOT}/results/occ_analysis_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUTPUT_DIR"

CONFIG_FILE="${1:-config/occ.yml}"

echo "=========================================="
echo "OCC Performance Analysis"
echo "=========================================="
echo "Output directory: $OUTPUT_DIR"
echo "Config file: $CONFIG_FILE"
echo ""

# Build with analysis enabled
echo "Building with OCC analysis enabled..."
cd build
cmake -DOCC_ANALYSIS_ENABLED=ON ..
make -j$(nproc) dbtest
cd ..

# Function to run analysis with specific parameters
run_analysis() {
    local test_name=$1
    local warehouses=$2
    local threads=$3
    local zipf_theta=$4
    local runtime=${5:-60}
    
    echo "----------------------------------------"
    echo "Running: $test_name"
    echo "  Warehouses: $warehouses"
    echo "  Threads: $threads"
    echo "  Zipf Theta: $zipf_theta"
    echo "  Runtime: ${runtime}s"
    echo "----------------------------------------"
    
    local output_file="${OUTPUT_DIR}/${test_name}.log"
    local stats_file="${OUTPUT_DIR}/${test_name}_stats.txt"
    local perf_file="${OUTPUT_DIR}/${test_name}_perf.csv"
    local false_abort_file="${OUTPUT_DIR}/${test_name}_false_aborts.csv"
    
    # Run benchmark
    ./build/dbtest \
        --bench tpcc \
        --config "$CONFIG_FILE" \
        --num-threads $threads \
        --runtime $runtime \
        --warehouses $warehouses \
        --zipf-theta $zipf_theta \
        > "$output_file" 2>&1 || true
    
    # Extract statistics (if analysis tools print them)
    echo "Analysis results saved to:"
    echo "  - Log: $output_file"
    echo "  - Stats: $stats_file"
    echo ""
}

# Test scenarios with varying contention
echo "Starting analysis runs..."
echo ""

# Low contention
run_analysis "low_contention" 10 24 0.0 60

# Medium contention
run_analysis "medium_contention" 10 48 0.5 60

# High contention
run_analysis "high_contention" 10 96 0.9 60

# Very high contention (hotspot)
run_analysis "hotspot" 10 96 0.99 60

echo "=========================================="
echo "Analysis complete!"
echo "Results saved to: $OUTPUT_DIR"
echo "=========================================="

# Generate summary report
cat > "${OUTPUT_DIR}/summary.md" << EOF
# OCC Analysis Summary

Generated: $(date)

## Test Configurations

1. **Low Contention**
   - Warehouses: 10
   - Threads: 24
   - Zipf Theta: 0.0 (uniform)
   - Results: \`low_contention_*\`

2. **Medium Contention**
   - Warehouses: 10
   - Threads: 48
   - Zipf Theta: 0.5
   - Results: \`medium_contention_*\`

3. **High Contention**
   - Warehouses: 10
   - Threads: 96
   - Zipf Theta: 0.9
   - Results: \`high_contention_*\`

4. **Hotspot**
   - Warehouses: 10
   - Threads: 96
   - Zipf Theta: 0.99 (highly skewed)
   - Results: \`hotspot_*\`

## Next Steps

1. Review log files for detailed output
2. Analyze performance CSV files
3. Check false abort analysis files
4. Compare abort rates across contention levels
EOF

echo "Summary report: ${OUTPUT_DIR}/summary.md"

