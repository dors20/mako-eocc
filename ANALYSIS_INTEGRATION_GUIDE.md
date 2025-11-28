# OCC Analysis Integration Guide

This guide shows exactly how to integrate the analysis tools into the existing OCC scheduler.

## Integration Steps

### Step 1: Add Analysis Header

Modify `src/deptran/occ/scheduler.cc`:

```cpp
#include "../__dep__.h"
#include "../scheduler.h"
#include "../config.h"
#include "tx.h"
#include "scheduler.h"

// ADD THIS:
#ifdef OCC_ANALYSIS_ENABLED
#include "occ_analysis.h"
#endif
```

### Step 2: Instrument DoPrepare() Method

Modify `SchedulerOcc::DoPrepare()` in `src/deptran/occ/scheduler.cc`:

```cpp
bool SchedulerOcc::DoPrepare(txnid_t tx_id) {
#ifdef OCC_ANALYSIS_ENABLED
    // Start profiling
    auto& profiler = PerformanceProfiler::getInstance();
    profiler.startValidation(tx_id);
#endif

    auto tx_box = dynamic_pointer_cast<TxOcc>(GetOrCreateTx(tx_id));
    auto txn = (mdb::TxnOCC*) get_mdb_txn(tx_id);
    verify(txn != nullptr);
    verify(txn->outcome_ == symbol_t::NONE);
    verify(!txn->verified_);

#ifdef OCC_ANALYSIS_ENABLED
    // Record read/write sets for false abort analysis
    // (Helper function needed to extract keys)
    auto read_set = extractKeysFromReadSet(txn);
    auto write_set = extractKeysFromWriteSet(txn);
    FalseAbortAnalyzer::getInstance().recordReadWriteSets(tx_id, read_set, write_set);
    
    // Start version check timing
    profiler.startVersionCheck(tx_id);
#endif

    // only do version check on leader.
    if (tx_box->is_leader_hint_ && !txn->version_check()) {
#ifdef OCC_ANALYSIS_ENABLED
        profiler.endVersionCheck(tx_id);
        profiler.endValidation(tx_id, false);
        AbortTracker::getInstance().recordAbort(tx_id, 
            OCCAbortReason::VERSION_CONFLICT, 
            AbortType::VERSION_STALE);
#endif
        Log_debug("txn: occ validation failed. id %" PRIx64 "site: %x", 
                 (int64_t)tx_id, (int)this->site_id_);
        txn->__debug_abort_ = 1;
        return false;
    }

#ifdef OCC_ANALYSIS_ENABLED
    profiler.endVersionCheck(tx_id);
    profiler.startReadLock(tx_id);
#endif

    // now lock the commit
    for (auto &it : txn->ver_check_read_) {
        Row *row = it.first.row;
        auto *v_row = (VersionedRow *) row;
        Log_debug("r_lock row: %llx", row);
        
        if (!v_row->rlock_row_by(txn->id())) {
#ifdef OCC_ANALYSIS_ENABLED
            profiler.endReadLock(tx_id);
            profiler.endValidation(tx_id, false);
            AbortTracker::getInstance().recordAbort(tx_id,
                OCCAbortReason::READ_LOCK_FAILED,
                AbortType::LOCK_CONTENTION);
#endif
            // ... existing unlock code ...
            txn->__debug_abort_ = 1;
            return false;
        }
        insert_into_map(txn->locks_, row, -1);
    }

#ifdef OCC_ANALYSIS_ENABLED
    profiler.endReadLock(tx_id);
    profiler.startWriteLock(tx_id);
#endif

    for (auto &it : txn->updates_) {
        Row *row = it.first;
        auto v_row = (VersionedRow *) row;
        Log_debug("w_lock row: %llx", row);
        
        if (!v_row->wlock_row_by(txn->id())) {
#ifdef OCC_ANALYSIS_ENABLED
            profiler.endWriteLock(tx_id);
            profiler.endValidation(tx_id, false);
            AbortTracker::getInstance().recordAbort(tx_id,
                OCCAbortReason::WRITE_LOCK_FAILED,
                AbortType::LOCK_CONTENTION);
#endif
            // ... existing unlock code ...
            txn->__debug_abort_ = 1;
            return false;
        }
        insert_into_map(txn->locks_, row, -1);
    }

#ifdef OCC_ANALYSIS_ENABLED
    profiler.endWriteLock(tx_id);
    profiler.endValidation(tx_id, true);
    AbortTracker::getInstance().recordCommit(tx_id);
#endif

    Log_debug("txn: %llx occ locks succeed.", (int64_t)tx_id);
    txn->__debug_abort_ = 0;
    txn->verified_ = true;
    return true;
}
```

### Step 3: Add Helper Functions

Add to `src/deptran/occ/scheduler.cc`:

```cpp
#ifdef OCC_ANALYSIS_ENABLED
// Helper functions to extract keys from read/write sets
static std::vector<std::string> extractKeysFromReadSet(const mdb::TxnOCC* txn) {
    std::vector<std::string> keys;
    for (const auto& it : txn->ver_check_read_) {
        Row* row = it.first.row;
        if (row && row->get_table()) {
            std::stringstream ss;
            ss << row->get_table()->Name() << ":" << (void*)row;
            keys.push_back(ss.str());
        }
    }
    return keys;
}

static std::vector<std::string> extractKeysFromWriteSet(const mdb::TxnOCC* txn) {
    std::vector<std::string> keys;
    for (const auto& it : txn->updates_) {
        Row* row = it.first;
        if (row && row->get_table()) {
            std::stringstream ss;
            ss << row->get_table()->Name() << ":" << (void*)row;
            keys.push_back(ss.str());
        }
    }
    return keys;
}
#endif
```

### Step 4: Add Print Statistics Hook

Add a function to print statistics at the end of benchmark:

In `src/deptran/occ/scheduler.h`:

```cpp
#ifdef OCC_ANALYSIS_ENABLED
    void printAnalysisStatistics() const;
#endif
```

In `src/deptran/occ/scheduler.cc`:

```cpp
#ifdef OCC_ANALYSIS_ENABLED
void SchedulerOcc::printAnalysisStatistics() const {
    AbortTracker::getInstance().printStatistics();
    PerformanceProfiler::getInstance().printStatistics();
    FalseAbortAnalyzer::getInstance().printAnalysis();
    
    // Write to files
    std::string prefix = "occ_analysis_";
    AbortTracker::getInstance().writeStatisticsToFile(prefix + "aborts.txt");
    PerformanceProfiler::getInstance().writeStatisticsToFile(prefix + "perf.csv");
    FalseAbortAnalyzer::getInstance().writeAnalysisToFile(prefix + "false_aborts.csv");
}
#endif
```

### Step 5: Update CMakeLists.txt

Add analysis source files and define flag:

```cmake
# In CMakeLists.txt, find the OCC source files section and add:

if(OCC_ANALYSIS_ENABLED)
    list(APPEND DEPTRAN_SRC
        "src/deptran/occ/occ_analysis.cc"
    )
    add_definitions(-DOCC_ANALYSIS_ENABLED)
    message(STATUS "OCC Analysis enabled")
endif()
```

### Step 6: Build and Test

```bash
# Build with analysis
cd build
cmake -DOCC_ANALYSIS_ENABLED=ON ..
make -j$(nproc) dbtest

# Run test
./build/dbtest --bench tpcc --config config/occ.yml --num-threads 24 --runtime 10

# Statistics should be printed at the end
```

## Minimal Integration (Quick Test)

For a quick test without modifying scheduler.cc, you can:

1. **Use the instrumented version temporarily**:
```bash
cd src/deptran/occ
cp scheduler.cc scheduler_original.cc.bak
cp scheduler_instrumented.cc scheduler.cc
```

2. **Build and run**:
```bash
cd build
cmake -DOCC_ANALYSIS_ENABLED=ON ..
make -j$(nproc) dbtest
cd ..
./build/dbtest --bench tpcc --config config/occ.yml --num-threads 24 --runtime 10
```

3. **Restore original**:
```bash
cd src/deptran/occ
mv scheduler_original.cc.bak scheduler.cc
```

## Verification

After integration, you should see output like:

```
========== OCC Abort Statistics ==========
Total Validations: 10000
Total Commits: 8500
Total Aborts: 1500
Abort Rate: 15.00%
...
```

If you don't see this output, check:
1. `OCC_ANALYSIS_ENABLED` is defined
2. Analysis code is being executed (add debug prints)
3. Statistics are printed at end of benchmark

## Next Steps

Once analysis is working:
1. Run full analysis suite: `./scripts/run_occ_analysis.sh`
2. Analyze results: `python3 scripts/analyze_occ_results.py results/...`
3. Identify bottlenecks and false abort opportunities
4. Plan enhancements based on findings

