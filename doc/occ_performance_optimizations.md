## OCC Batching & Reordering – Performance Optimization Ideas

This document captures concrete performance optimizations for the OCC batching + Ding-style reordering implementation on `mako-txn-reordering`, based on analysis of the `results/occ_runs/final` TPCC runs on a 32‑core machine.

The primary goals are:

- Get **batch validation** throughput back to parity or better vs the baseline OCC implementation.
- Make **storage reordering** beneficial (as in Ding et al.), instead of a throughput killer.

---

### 1. Parameter Tuning (No Code Changes)

These optimizations only change environment variables / YAML configs.

#### 1.1 Validator Batch Size and Wait Time

Current TPCC settings (e.g., `dbtest_batch_validation_tpcc`):

- `MAKO_BATCH_VALIDATION_SIZE=64`
- `MAKO_BATCH_VALIDATION_MAX_WAIT_US=5000`

Observed behaviour at medium contention (16 threads):

- `avg_batch_size` ≈ 16, `avg_batch_validation_time_us` ≈ 47 µs.
- Per-txn latency grows from ~0.013 ms (baseline) to ~0.105 ms (~8×).
- Throughput drops from ~75K txns/s to ~9.5K txns/s (~8×).

**Hypothesis & Fix:**

- The validator becomes a **blocking barrier** for commits. With a 5 ms wait, threads often stall waiting for batches to fill.
- Reduce wait time and batch size so batch validation behaves as an **opportunistic acceleration** rather than a mandatory barrier.

**Suggested TPCC defaults:**

```bash
export MAKO_BATCH_VALIDATION_SIZE=16         # or 32
export MAKO_BATCH_VALIDATION_MAX_WAIT_US=250 # 100–250 µs range
```

This should:

- Keep coordination latency roughly within a single RPC / cache-miss scale.
- Still allow batching benefits when traffic is high.

#### 1.2 Storage Reorder Batch Size and Wait Time

Current storage scenario (`dbtest_batch_validation_storage`):

- `MAKO_ENABLE_STORAGE_REORDER=1`
- `MAKO_STORAGE_REORDER_SIZE=64`
- `MAKO_STORAGE_REORDER_MAX_WAIT_US=2000`

Observed behaviour:

- `storage_reorder_phase_finalize_us` ≈ 4,586 µs per batch.
- Throughput: ~2.2K ops/s vs ~75K baseline; avg latency ~7.2 ms vs 0.013 ms.

**Hypothesis & Fix:**

- Each commit is effectively queued behind **two separate batching layers**:
  - Storage-level (`StoStorageReorderer`).
  - Validator-level (`StoBatchValidator`).
- Combined wait times (2 ms + up to 5 ms) dominate end-to-end latency.

**Suggested TPCC defaults:**

```bash
export MAKO_STORAGE_REORDER_SIZE=16
export MAKO_STORAGE_REORDER_MAX_WAIT_US=50   # or even 0 for “flush asap”
```

The goal is to:

- Preserve the Ding-style **intra-batch reordering** benefit.
- Make storage reordering’s own queuing/coordination cost negligible vs compute.

---

### 2. Behavioural Knobs (Still Config-Level)

#### 2.1 When to Enable Reordering

Reordering has clear wins in Ding et al. under **high contention**, but adds overhead at low/medium contention.

**Strategy:**

- Use **batch validation only** (no reordering) for:
  - Low/medium contention TPCC curves.
  - “Production” performance baselines.
- Use **reordering** only in:
  - High-contention stress tests (e.g., synthetic skew, small keyspace).
  - Targeted experiments evaluating Ding-style algorithms.

Practically, this means:

- Set `MAKO_ENABLE_TXN_REORDER=0` in default TPCC configs.
- Turn it on only in explicitly high-contention scenarios.

#### 2.2 Gating Storage Reorder by Contention

To keep Ding’s “storage batching is most beneficial” spirit but avoid constant overhead:

- Add a simple contention gate (future work):
  - E.g., enable storage reordering only if moving average of:
    - `agg_abort_rate_per_sec` or
    - `storage_reorder_cycles_detected`
  - exceeds a threshold for some window.

---

### 3. Low‑Impact Code Changes

These keep semantics identical but reduce unnecessary blocking / waiting.

#### 3.1 Make Storage Reorder Less Blocking

Current behaviour (simplified):

- `StoStorageReorderer::Process(txn)`:
  - Enqueues `Entry` into `pending_`.
  - Worker thread eventually calls `Flush(ready_entries)`.
  - `Flush()`:
    - Runs FVS-based reordering.
    - Calls `StoBatchValidator::Instance().ProcessBatch(survivor_txns, decisions)` **synchronously**.
    - Only after validator finishes are entries unblocked.

Result: storage reorder’s `phase_finalize_us` includes **all** of validator queuing / processing time, leading to multi-ms latencies.

**Optimization sketch:**

- Make `Flush()`:
  - Enqueue to validator (`StoBatchValidator::ProcessBatch`-like API), but
  - **Return quickly** and let validator complete asynchronously.
- Transactions would then:
  - Block at most on one queue (validator), not both queue layers serially.

This would require:

- A non-blocking validator API with callbacks or future/promise.
- Slight refactoring of how the STO commit path waits for results.

#### 3.2 Opportunistic (Fallback-Friendly) Batch Validation

Current `BatchValidator::AddToBatch` behaviour:

- Every commit de-facto passes through `AddToBatch` and may:
  - Wait for the batch to fill or time out,
  - Or block on another thread’s batch flush.

**Optimization sketch:**

- If `AddToBatch` does **not** find a ready-to-flush batch:
  - Immediately return “not batched,” and
  - Let the calling thread run the **original sequential validation path**.

This preserves:

- All current correctness guarantees.
- Existing fast path for high-traffic situations (batches fill quickly).

But avoids:

- Turning batching into a global barrier under light/medium traffic.

---

### 4. Architectural Improvements (Longer‑Term)

These align directly with Ding et al.’s architecture but are more invasive.

#### 4.1 Dedicated Parallel Validator Component

Instead of:

- Each worker thread running validation (and optionally batching) within its commit path.

Move to:

- A **dedicated validator “service”** with:
  - Input queues per core or per-partition.
  - Batch construction + FVS reordering + validation executed by validator workers.
- Worker threads:
  - Enqueue commit requests to validator.
  - Continue generating new txns (subject to a concurrency cap).
  - Receive commit/abort decisions asynchronously.

This would:

- Unlock higher CPU utilisation at the validator.
- Match Ding et al.’s parallel validator more closely.

#### 4.2 True Storage‑Level Batching per Key

Longer term, to really realize Ding’s storage win:

- Buffer read/write requests per key at the storage layer (in TPCC’s Masstree / RocksDB front end).
- For each batch:
  - Apply highest-version writes for each key first.
  - Then process reads.

The current `StoStorageReorderer` is a **transaction‑level** reorderer, not a key‑level storage engine reorderer. It is a safe first step but may not reach Ding’s full benefit until more of the write/read scheduling is pushed into the storage engine.

---

### 5. Summary

- The **throughput collapse** observed on 32‑core TPCC runs is dominated by:
  - Overly large `*_MAX_WAIT_US` parameters (especially for storage reorder).
  - Strictly blocking batchers at multiple stages.
- We can likely recover most of the baseline throughput by:
  - Tuning batch sizes and wait times down,
  - Avoiding storage reorder at low/medium contention (or gating it by contention),
  - Gradually relaxing the strict blocking semantics of `Flush()` and `AddToBatch`.
- Longer‑term, a dedicated validator and key‑level storage batching would line Mako up much more closely with Ding et al.’s architecture and expected performance wins, especially for storage reordering.


