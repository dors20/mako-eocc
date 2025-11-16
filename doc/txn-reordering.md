## Goal

Implement the transaction batching and reordering framework from Ding *et al.*, “Improving Optimistic Concurrency Control Through Transaction Batching and Operation Reordering,” within Mako while keeping the design modular, RustyCpp-compliant, and configurable at runtime.

---

## Paper Summary

1. **Problem**  
   - OCC wastes work under contention because conflicts are detected only at validation.  
   - Semantic batching plus reordering at storage and validator can reduce conflicts and tail latency.

2. **Key Components**  
   - **Storage batching:** buffer reads/writes per key; apply highest-version writes first, then reads on up-to-date versions.  
   - **Validator batching (IBVR):** batch validation requests, build dependency graph, compute Feedback Vertex Set (FVS) to decide which transactions must abort; commit remaining transactions in dependency order.  
   - **Policies:** minimize abort count, weighted priorities, tail-latency aware, thread-aware heuristic for multi-threaded validator.  
   - **Algorithms:** SCC-based greedy FVS, sort-based greedy, hybrid heuristics.

3. **Expected Results**  
   - Up to 2.2× throughput gains and 71% tail-latency reduction under high contention.  
   - Weighted policies can trade off fairness vs throughput.  
   - Need to evaluate sensitivity to batch sizes, contention levels, workload mixes.

---

## Implementation Tasks

### 1. Architecture & Config Surface
- [ ] Introduce a top-level `TxnReorderingConfig` (JSON-driven via CLI flag) with toggles for `storage_batching`, `validator_batching`, and policy parameters.
- [ ] Integrate config plumbing into `dbtest` and `config/*.yml` loader so the feature can be enabled/disabled per run without recompilation.
- [ ] Ensure config is accessible inside Mako’s scheduler/validator components (likely `src/deptran/occ` and `src/mako/lib`).

### 2. Storage Batching Module
- [ ] Identify storage-facing APIs (Masstree access paths in `src/mako/*` and `src/deptran/*`).  
- [ ] Design per-key (or per-shard) batching buffers; reuse existing per-core queues if possible.  
- [ ] Implement write-first ordering: apply highest timestamp write, then service read queue with up-to-date value.  
- [ ] Provide knobs: `storage_batch_size`, `flush_interval_us`, `max_batch_bytes`.  
- [ ] Add instrumentation counters (batched ops, stale-read avoidance).  
- [ ] Guard entire module behind feature flag; default off.

### 3. Validator Batching & Reordering
- [ ] Extend OCC validator to collect `Batch` objects (configurable `validator_batch_size`, `max_wait_us`).  
- [ ] Build dependency graph per batch: nodes = transactions, edges = read-after-write conflicts.  
- [ ] Implement greedy FVS algorithms described in the paper:  
  - SCC-based greedy (default).  
  - Sort-based heuristic (optional).  
- [ ] Support policy hooks:
  - `min_abort` (pure throughput).  
  - `age_priority` (reward retries).  
  - `tail_latency` / `deadline`.  
  - `thread_aware` (avoid starving certain cores).  
- [ ] Expose policy selection + weights via JSON config.  
- [ ] Ensure graph construction & reordering uses RustyCpp smart pointers (no raw `new`).  
- [ ] Emit metrics: batch size, FVS size, abort causes, reorder time.

### 4. Hyper-Parameter Plumbing
- [ ] Define JSON schema (document in `experiments.md`):
  ```json
  {
    "txn_reordering": {
      "enabled": true,
      "storage_batching": { "enabled": true, "max_batch": 64, "flush_interval_us": 50 },
      "validator_batching": {
        "enabled": true,
        "batch_size": 64,
        "policy": "min_abort",
        "policy_params": { "age_weight": 0.2 },
        "algorithm": "scc_greedy"
      }
    }
  }
  ```
- [ ] Add CLI flag `--txn-config=/path/to/json`. If absent, fall back to defaults (feature off).
- [ ] Validate JSON at startup; fail fast with descriptive error.

### 5. RustyCpp & Safety
- [ ] Use RustyCpp smart pointers for new data structures (graphs, batches).  
- [ ] Annotate unsafe sections with `// @unsafe` and justify.  
- [ ] Add unit tests for borrow-checker (e.g., `make borrow_check_all_dbtest`).

### 6. Telemetry & Logging
- [ ] Integrate with existing stats (`results/` and `scripts/aggregate`).  
- [ ] Log reordering decisions when `MAKO_LOG_LEVEL=debug`.  
- [ ] Export JSON snippets summarizing each batch for offline analysis.

### 7. Documentation
- [ ] Cross-link config options in `doc/config.md`.  
- [ ] Update `commands.md` / `run.md` with new CLI flag usage.  
- [ ] Add section to `doc/mako-eocc-design.md` describing the module.

### 8. Validation Plan
- [ ] Unit tests for graph builder & policies (synthetic workloads).  
- [ ] Integration tests: run `ci/ci.sh simpleTransaction` with small batches to ensure no regression when feature disabled/enabled.  
- [ ] Stress test: adapt `examples/test_2shard_replication.sh` to toggle feature.  
- [ ] Verify that disabling returns to baseline behavior and metrics.

---

## Open Questions / Risks
- How to align batching windows with existing per-core schedulers without hurting latency?  
- Interaction with speculative replication and vector watermarks—need to ensure reordered commits still advance watermarks correctly.  
- Potential memory overhead of graph construction at high concurrency; may need incremental processing.

---

## Next Steps
1. Finalize JSON schema and plumbing (Task 1 & 4).  
2. Prototype storage batching pipeline + metrics.  
3. Implement validator graph builder & SCC greedy policy.  
4. Stand up experiments from `doc/experiments.md`.  
5. Iterate on performance + memory profiling.

