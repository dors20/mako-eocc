#!/bin/bash
# Quick test to see OCC analysis output

cd /home/ubuntu/mako

echo "=========================================="
echo "Testing OCC Analysis - Quick Test"
echo "=========================================="
echo ""

# Check if built with analysis
if ! grep -q "OCC_ANALYSIS_ENABLED" build/CMakeCache.txt 2>/dev/null; then
    echo "⚠️  Building with OCC_ANALYSIS_ENABLED..."
    cd build
    cmake -DOCC_ANALYSIS_ENABLED=ON .. > /dev/null 2>&1
    make -j$(nproc) dbtest > /dev/null 2>&1
    cd ..
    echo "✅ Build complete"
fi

echo ""
echo "To see OCC analysis output, you need to run a benchmark that:"
echo "  1. Uses OCC (config/occ.yml)"
echo "  2. Goes through deptran framework"
echo ""
echo "Option 1: Run simplePaxos example (uses OCC):"
echo "  ./examples/simplePaxos.sh"
echo ""
echo "Option 2: Run deptran_server directly:"
echo "  ./build/deptran_server -f config/occ.yml -f config/1c1s1p.yml -f config/tpca.yml -P localhost -d 10"
echo ""
echo "Option 3: Use test_run.py:"
echo "  python3 test_run.py  # (if configured)"
echo ""
echo "The analysis statistics will print automatically at the end!"
echo ""
echo "=========================================="
echo ""
echo "Running simplePaxos test now..."
echo ""

# Try running simplePaxos
if [ -f "./examples/simplePaxos.sh" ]; then
    timeout 30 bash ./examples/simplePaxos.sh 2>&1 | tee /tmp/occ_test_run.log | tail -50
    echo ""
    echo "Checking for analysis output..."
    if grep -q "OCC Abort Statistics" /tmp/occ_test_run.log; then
        echo "✅ Found analysis output!"
        grep -A 20 "OCC Abort Statistics" /tmp/occ_test_run.log
    else
        echo "⚠️  No analysis output found yet."
        echo "   This might be because:"
        echo "   - No OCC transactions were executed"
        echo "   - Benchmark didn't use OCC scheduler"
        echo "   - Scheduler wasn't destroyed (stats print on destruction)"
    fi
else
    echo "simplePaxos.sh not found. Try running deptran_server manually."
fi

