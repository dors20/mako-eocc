# OCC Execution Path Analysis

## Complete Execution Flow

### 1. Scheduler Creation (Server Startup)

**Location:** `src/deptran/server_worker.cc:50`
```cpp
tx_sched_ = tx_frame_->CreateScheduler();
```

**Location:** `src/deptran/frame.cc:344-352`
```cpp
TxLogServer* Frame::CreateScheduler() {
  auto mode = Config::GetConfig()->tx_proto_;
  switch(mode) {
    case MODE_OCC:
      sch = new SchedulerOcc();  // ← OCC scheduler created here
      break;
    ...
  }
}
```

**When:** This happens when config file has `cc: occ`

### 2. Service Registration (RPC Handler Setup)

**Location:** `src/deptran/server_worker.cc:139`
```cpp
services_ = tx_frame_->CreateRpcServices(site_info_->id,
                                         tx_sched_,  // ← SchedulerOcc* passed here
                                         ...);
```

**Location:** `src/deptran/frame.cc:413`
```cpp
result.push_back(new ClassicServiceImpl(dtxn_sched, ...));
// dtxn_sched is the SchedulerOcc* instance
```

### 3. RPC Request Arrives

**Location:** `src/deptran/service.cc:78-89`
```cpp
void ClassicServiceImpl::Prepare(const rrr::i64& tid,
                                 const std::vector<i32>& sids,
                                 rrr::i32* res,
                                 rrr::DeferredReply* defer) {
  const auto& func = [res, defer, tid, sids, this]() {
    auto sched = (SchedulerClassic*) dtxn_sched_;  // ← Cast to base class
    bool ret = sched->OnPrepare(tid, sids);  // ← Calls OnPrepare
    *res = ret ? SUCCESS : REJECT;
    defer->reply();
  };
  Coroutine::CreateRun(func);
}
```

### 4. OnPrepare Calls DoPrepare

**Location:** `src/deptran/classic/scheduler.cc:143-170`
```cpp
bool SchedulerClassic::OnPrepare(cmdid_t tx_id,
                                 const std::vector<i32>& sids) {
  ...
  if (Config::GetConfig()->IsReplicated()) {
    // Paxos replication path
    ...
  } else {
    return DoPrepare(tx_id);  // ← Virtual call, dispatches to SchedulerOcc::DoPrepare()
  }
}
```

**Also:** `src/deptran/classic/scheduler.cc:188` (replicated path)
```cpp
sp_tx->result_prepare_ = DoPrepare(sp_tx->tid_);  // ← Also calls DoPrepare
```

### 5. OCC DoPrepare Executes (OUR INSTRUMENTATION!)

**Location:** `src/deptran/occ/scheduler.cc:54`
```cpp
bool SchedulerOcc::DoPrepare(txnid_t tx_id) {
#ifdef OCC_ANALYSIS_ENABLED
  auto& profiler = PerformanceProfiler::getInstance();
  profiler.startValidation(tx_id);  // ← INSTRUMENTATION STARTS HERE
  profiler.startVersionCheck(tx_id);
#endif
  ...
  // Version check
  if (tx_box->is_leader_hint_ && !txn->version_check()) {
    // Abort path - instrumentation records abort
  }
  ...
  // Lock acquisition
  ...
  // Success path - instrumentation records commit
}
```

## Key Requirements for OCC Execution

1. **Config must specify OCC:**
   - Config file must have `cc: occ` or `mode: { cc: occ }`
   - This sets `Config::tx_proto_ = MODE_OCC`

2. **Server must be running:**
   - `ServerWorker::SetupBase()` creates scheduler
   - `ServerWorker::SetupService()` registers RPC handlers
   - RPC server must be listening

3. **Prepare RPC must be received:**
   - Client/Coordinator sends Prepare RPC
   - `ClassicServiceImpl::Prepare()` handles it
   - This triggers the execution path

4. **Transaction must be on leader (for replication):**
   - `tx_box->is_leader_hint_` must be true
   - Otherwise version check is skipped

## Why Analysis Tools Aren't Showing Output

### Problem 1: No Prepare RPCs Received
- If no clients are sending transactions
- If RPC server isn't running
- If transactions aren't reaching prepare phase

### Problem 2: Not Using OCC Mode
- Config might not have `cc: occ`
- Might be using different scheduler (2PL, etc.)

### Problem 3: Transactions Not on Leader
- If `is_leader_hint_` is false, version check is skipped
- But locks are still acquired, so some instrumentation runs

### Problem 4: Benchmark Crashes Before Completion
- Stats print in destructor/shutdown
- If process crashes, destructor might not run

## Verification Checklist

✅ **Scheduler Created?**
- Check if `SchedulerOcc` constructor runs
- Add log in constructor

✅ **RPC Server Running?**
- Check if `rpc_server_->start()` succeeds
- Check if services are registered

✅ **Prepare RPCs Received?**
- Add log in `ClassicServiceImpl::Prepare()`
- Check if `OnPrepare()` is called

✅ **DoPrepare Called?**
- Add log at start of `SchedulerOcc::DoPrepare()`
- This is where instrumentation starts

✅ **Config Correct?**
- Verify config file has `cc: occ`
- Verify `tx_proto_ == MODE_OCC`

## Next Steps to Debug

1. **Add debug logs:**
   ```cpp
   // In SchedulerOcc::DoPrepare()
   Log_info("OCC DoPrepare called for tx_id: %" PRIx64, tx_id);
   ```

2. **Check if scheduler is OCC:**
   ```cpp
   // In ServerWorker::SetupBase()
   Log_info("Created scheduler type: %d", tx_sched_->get_mode());
   ```

3. **Verify RPC calls:**
   ```cpp
   // In ClassicServiceImpl::Prepare()
   Log_info("Prepare RPC received for tx_id: %" PRIx64, tid);
   ```

4. **Check config:**
   ```cpp
   // In Frame::CreateScheduler()
   Log_info("Creating scheduler for mode: %d", mode);
   ```

## Summary

**Execution Path:**
```
Config (cc: occ) 
  → Frame::CreateScheduler() creates SchedulerOcc
  → ServerWorker stores in tx_sched_
  → ClassicServiceImpl receives Prepare RPC
  → SchedulerClassic::OnPrepare() called
  → SchedulerOcc::DoPrepare() executes ← INSTRUMENTATION HERE
  → Stats collected
  → Stats printed on shutdown/destructor
```

**The instrumentation IS in the right place!** It just needs:
1. OCC mode enabled in config
2. Prepare RPCs to arrive
3. Transactions to execute
4. Process to complete (not crash)

