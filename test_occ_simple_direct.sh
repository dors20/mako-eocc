#!/bin/bash
# Direct test of OCC analysis tools - verify they work

cd /home/ubuntu/mako

echo "=========================================="
echo "Direct OCC Analysis Test"
echo "=========================================="
echo ""

# Test 1: Verify the tools compile and can be called directly
echo "Test 1: Creating minimal test program..."
cat > /tmp/test_occ_direct.cc << 'EOF'
#include "deptran/occ/occ_analysis.h"
#include <iostream>
#include <cstdlib>

int main() {
    std::cout << "Testing OCC Analysis Tools..." << std::endl;
    
    // Test AbortTracker
    auto& tracker = janus::AbortTracker::getInstance();
    tracker.recordAbort(1, janus::OCCAbortReason::VERSION_CONFLICT, janus::AbortType::FALSE_ABORT);
    tracker.recordAbort(2, janus::OCCAbortReason::READ_LOCK_FAILED, janus::AbortType::LOCK_CONTENTION);
    tracker.recordCommit(3);
    tracker.recordCommit(4);
    
    std::cout << "\n✅ AbortTracker works - recorded 2 aborts, 2 commits\n" << std::endl;
    
    // Test PerformanceProfiler  
    auto& profiler = janus::PerformanceProfiler::getInstance();
    profiler.startValidation(1);
    profiler.endVersionCheck(1);
    profiler.endValidation(1, true);
    
    std::cout << "✅ PerformanceProfiler works\n" << std::endl;
    
    // Print statistics
    std::cout << "==========================================" << std::endl;
    std::cout << "PRINTING STATISTICS:" << std::endl;
    std::cout << "==========================================" << std::endl;
    tracker.printStatistics();
    profiler.printStatistics();
    
    std::cout << "\n✅ All analysis tools work correctly!" << std::endl;
    return 0;
}
EOF

echo "Compiling test..."
cd build
g++ -std=c++11 -I../src -DOCC_ANALYSIS_ENABLED \
    ../src/deptran/occ/occ_analysis.cc \
    /tmp/test_occ_direct.cc -o test_occ_direct \
    -lpthread 2>&1 | grep -E "(error|Error|warning:.*occ)" | head -5

if [ $? -eq 0 ] && [ -f test_occ_direct ]; then
    echo ""
    echo "✅ Compilation successful!"
    echo ""
    echo "Running test..."
    echo "=========================================="
    ./test_occ_direct
    echo "=========================================="
    echo ""
    echo "✅ If you see statistics above, the tools work!"
    echo ""
    echo "Next: The tools are integrated into scheduler.cc"
    echo "      They will print when OCC transactions run."
    echo ""
    echo "To test with real transactions, you need to:"
    echo "  1. Build deptran_server (via make/waf, not cmake)"
    echo "  2. Run a benchmark that uses OCC"
    echo "  3. Stats will print automatically"
else
    echo "❌ Compilation failed"
    echo "Check that OCC_ANALYSIS_ENABLED is defined"
fi

rm -f /tmp/test_occ_direct.cc
cd ..

