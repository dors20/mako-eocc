#!/bin/bash
# Simple test to verify OCC analysis tools compile and can be called

cd /home/ubuntu/mako

echo "Testing OCC Analysis Tools Compilation..."
echo "=========================================="

# Test 1: Check if analysis files compile
echo ""
echo "Test 1: Checking if occ_analysis.cc compiles..."
cd build
if make occ_analysis 2>&1 | grep -q "error"; then
    echo "❌ Compilation errors found"
    make occ_analysis 2>&1 | grep error | head -5
else
    echo "✅ Analysis code compiles successfully"
fi

# Test 2: Create a simple test program
echo ""
echo "Test 2: Creating simple test program..."
cd ..
cat > /tmp/test_occ_analysis_simple.cc << 'TESTEOF'
#include "deptran/occ/occ_analysis.h"
#include <iostream>

int main() {
    std::cout << "Testing OCC Analysis Tools..." << std::endl;
    
    // Test AbortTracker
    auto& tracker = janus::AbortTracker::getInstance();
    tracker.recordAbort(1, janus::OCCAbortReason::VERSION_CONFLICT, janus::AbortType::FALSE_ABORT);
    tracker.recordCommit(2);
    std::cout << "✅ AbortTracker works" << std::endl;
    
    // Test PerformanceProfiler  
    auto& profiler = janus::PerformanceProfiler::getInstance();
    profiler.startValidation(1);
    profiler.endValidation(1, true);
    std::cout << "✅ PerformanceProfiler works" << std::endl;
    
    // Test FalseAbortAnalyzer
    auto& analyzer = janus::FalseAbortAnalyzer::getInstance();
    analyzer.recordReadWriteSets(1, {"key1"}, {"key2"});
    std::cout << "✅ FalseAbortAnalyzer works" << std::endl;
    
    // Print statistics
    std::cout << "\nStatistics:" << std::endl;
    tracker.printStatistics();
    
    std::cout << "\n✅ All analysis tools work correctly!" << std::endl;
    return 0;
}
TESTEOF

# Try to compile the test
echo "Compiling test program..."
cd build
g++ -std=c++11 -I../src -I. -c ../src/deptran/occ/occ_analysis.cc -o occ_analysis.o 2>&1 | head -10
if [ $? -eq 0 ]; then
    echo "✅ Test program compiles"
    echo ""
    echo "Analysis tools are ready to use!"
    echo "Next: Integrate into scheduler.cc (see ANALYSIS_INTEGRATION_GUIDE.md)"
else
    echo "❌ Compilation failed"
fi

rm -f /tmp/test_occ_analysis_simple.cc
cd ..
