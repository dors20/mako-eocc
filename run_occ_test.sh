#!/bin/bash
# Run OCC test with deptran_server to trigger OCC execution

cd /home/ubuntu/mako

echo "=========================================="
echo "Running OCC Test with deptran_server"
echo "=========================================="
echo ""

# Build deptran_server if needed
if [ ! -f "build/deptran_server" ]; then
    echo "Building deptran_server..."
    cd build
    cmake -DOCC_ANALYSIS_ENABLED=ON ..
    make -j$(nproc) deptran_server
    cd ..
    echo "✅ Build complete"
fi

echo ""
echo "Running deptran_server with OCC configuration..."
echo "This will:"
echo "  1. Start server with OCC scheduler"
echo "  2. Start client workers"
echo "  3. Execute transactions for 10 seconds"
echo "  4. Print OCC analysis statistics at the end"
echo ""
echo "Command:"
echo "  ./build/deptran_server -f config/occ.yml -f config/1c1s1p.yml -f config/tpca.yml -P localhost -d 10"
echo ""
echo "=========================================="
echo ""

# Run the test
./build/deptran_server \
    -f config/occ.yml \
    -f config/1c1s1p.yml \
    -f config/tpca.yml \
    -P localhost \
    -d 10 2>&1 | tee /tmp/occ_deptran_test.log

echo ""
echo "=========================================="
echo "Checking for OCC Analysis Output..."
echo "=========================================="

if grep -q "OCC Abort Statistics" /tmp/occ_deptran_test.log; then
    echo "✅ FOUND ANALYSIS OUTPUT!"
    echo ""
    grep -A 30 "OCC Abort Statistics" /tmp/occ_deptran_test.log
else
    echo "⚠️  No analysis output found in log"
    echo ""
    echo "Checking if DoPrepare was called..."
    if grep -q "DoPrepare\|Prepare RPC\|occ validation" /tmp/occ_deptran_test.log; then
        echo "✅ Found OCC activity in logs"
        grep -i "prepare\|occ\|validation" /tmp/occ_deptran_test.log | tail -10
    else
        echo "❌ No OCC activity found"
        echo ""
        echo "Possible issues:"
        echo "  - Server didn't start"
        echo "  - No transactions were sent"
        echo "  - Config didn't use OCC mode"
    fi
fi

echo ""
echo "Full log saved to: /tmp/occ_deptran_test.log"

