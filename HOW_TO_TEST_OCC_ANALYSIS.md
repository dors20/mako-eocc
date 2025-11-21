# How to Test OCC Analysis After Building

## ✅ You've Already Built It!

```bash
cd build
cmake -DOCC_ANALYSIS_ENABLED=ON ..
make -j$(nproc) dbtest
cd ..
```

## 🚀 Next Steps: Run a Benchmark

The analysis tools are **integrated** and will automatically print statistics when:
1. OCC transactions run through `SchedulerOcc::DoPrepare()`
2. The scheduler is destroyed (end of benchmark) OR server shuts down

### Option 1: Run deptran_server (Recommended)

```bash
# Simple test - run for 10 seconds
./build/deptran_server \
    -f config/occ.yml \
    -f config/1c1s1p.yml \
    -f config/tpca.yml \
    -P localhost \
    -d 10
```

**Look for output like:**
```
========== OCC Abort Statistics ==========
Total Validations: 1000
Total Commits: 850
Total Aborts: 150
...
```

### Option 2: Check Log Files

If you ran a test that crashed (like simplePaxos), check the log files:

```bash
# Check if any OCC stats were printed
grep -r "OCC Abort Statistics" a*.log 2>/dev/null || echo "No stats found yet"
```

### Option 3: Manual Test (Verify Integration)

Create a simple test program to verify the tools work:

```bash
cat > /tmp/test_occ_tools.cc << 'EOF'
#include "deptran/occ/occ_analysis.h"
#include <iostream>

int main() {
    auto& tracker = janus::AbortTracker::getInstance();
    tracker.recordAbort(1, janus::OCCAbortReason::VERSION_CONFLICT);
    tracker.recordCommit(2);
    tracker.printStatistics();
    return 0;
}
EOF

cd build
g++ -std=c++11 -I../src -DOCC_ANALYSIS_ENABLED \
    ../src/deptran/occ/occ_analysis.cc \
    /tmp/test_occ_tools.cc -o test_occ_tools \
    -lpthread 2>&1 | head -5
./test_occ_tools
```

## 🔍 What Gets Tracked

When OCC transactions run, the tools automatically track:

- ✅ **Every validation** (`DoPrepare()` call)
- ✅ **Version check time**
- ✅ **Lock acquisition time** (read & write)
- ✅ **Abort reasons** (version conflict, lock failures)
- ✅ **Abort types** (real vs false aborts)
- ✅ **Commit/abort counts**

## 📊 Where Statistics Print

Statistics print in **two places**:

1. **Scheduler destructor** - When scheduler is destroyed
2. **ServerWorker::ShutDown()** - Before server shutdown (more reliable)

Both use `Log_info()` so they'll appear in logs even if stdout is redirected.

## 🐛 Troubleshooting

**No output?**
- Make sure `OCC_ANALYSIS_ENABLED` is defined (check `build/CMakeCache.txt`)
- Verify OCC transactions actually ran (check benchmark output)
- Check log files, not just stdout
- The test might have crashed before cleanup

**Test crashed?**
- Stats print in `ServerWorker::ShutDown()` which runs before crash cleanup
- Check log files for partial output
- Try a simpler benchmark (tpca instead of tpcc)

## ✅ Success Indicators

You'll know it's working when you see:

```
========== OCC Abort Statistics ==========
Total Validations: [number]
Total Commits: [number]
Total Aborts: [number]
Abort Rate: [percentage]%
...
========== OCC Performance Statistics ==========
...
```

## 🎯 Quick Test Command

```bash
# Run a 10-second OCC benchmark
timeout 15 ./build/deptran_server \
    -f config/occ.yml \
    -f config/1c1s1p.yml \
    -f config/tpca.yml \
    -P localhost \
    -d 10 2>&1 | grep -A 30 "OCC Abort Statistics"
```

If you see the statistics output, **it's working!** 🎉

