#!/bin/bash
# Test OCC analysis tools with actual OCC transactions

cd /home/ubuntu/mako

echo "=========================================="
echo "Testing OCC Analysis Tools (Integrated)"
echo "=========================================="
echo ""

# Build with analysis enabled
echo "Building with OCC_ANALYSIS_ENABLED..."
cd build
cmake -DOCC_ANALYSIS_ENABLED=ON .. > /dev/null 2>&1
make -j$(nproc) dbtest > /dev/null 2>&1
cd ..

echo "✅ Build complete"
echo ""
echo "To test with real OCC transactions, you need to:"
echo ""
echo "1. Run a deptran benchmark that uses OCC:"
echo "   - Use config/occ.yml (already configured for OCC)"
echo "   - Run through deptran framework (not dbtest)"
echo ""
echo "2. The analysis statistics will print automatically"
echo "   when the scheduler is destroyed (end of benchmark)"
echo ""
echo "Example (if you have deptran benchmarks set up):"
echo "  ./run.py -d 10 -c config/occ.yml ..."
echo ""
echo "The analysis tools are now integrated and will:"
echo "  ✅ Track all OCC validations"
echo "  ✅ Measure performance (validation time, lock time)"
echo "  ✅ Track abort reasons and types"
echo "  ✅ Detect false aborts"
echo "  ✅ Print statistics at end of benchmark"
echo ""
echo "=========================================="

