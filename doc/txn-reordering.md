## Goal

Implement the transaction batching and reordering framework from Ding *et al.*, “Improving Optimistic Concurrency Control Through Transaction Batching and Operation Reordering,” within Mako while keeping the design modular, RustyCpp-compliant, and configurable at runtime.

---

## Status (Nov 19, 2025)

- Added `txn_reordering` config loader backed by YAML/JSON and exposed via the new `--txn-config` flag in `dbtest`.
- Introduced `StorageBatcher` (writes-before-reads) and `ValidatorBatcher` (batch + policy ordering) inside the shard server pipeline; both honor the JSON hyper-parameters.
- Logging hooks and event counters now emit per-flush telemetry, plus a `RequestAccessTracker` that captures per-key read/write sets for every `req_nr`.
- Validator batches now build per-shard **directed** dependency graphs, run a greedy FVS policy (default: min-abort), and selectively abort conflicting validators before they reach Masstree.
- Structured counters feed both the stats server and a new `results/trcc_metrics.json` artifact, so experiments capture storage/validator batch sizes, drop counts, conflict density, and reorder latency without extra instrumentation.

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
- [x] Introduce a top-level `TxnReorderingConfig` (JSON-driven via CLI flag) with toggles for `storage_batching`, `validator_batching`, and policy parameters. ✅ implemented via `txn_reordering_config.{h,cc}` and the `--txn-config` flag.
- [x] Integrate config plumbing into `dbtest` so the feature can be enabled/disabled per run without recompilation (JSON parser piggybacks on YAML-cpp).
- [x] Ensure config is accessible inside Mako’s scheduler/validator components (wired through `ShardServer` + batchers).

### 2. Storage Batching Module
- [x] Identify storage-facing APIs (Masstree access paths in `src/mako/*` and `src/deptran/*`).  
- [ ] (Stretch) Design per-key (or per-shard) batching buffers; reuse existing per-core queues if possible.  
- [x] Implement write-first ordering: apply highest timestamp write, then service read queue with up-to-date value (initially at the request-class granularity; per-key refinement still open).  
- [x] Provide knobs: `storage_batch_size`, `flush_interval_us`, `max_batch_bytes`.  
- [x] Add instrumentation counters (batched ops, stale-read avoidance).  
- [x] Guard entire module behind feature flag; default off.

### 3. Validator Batching & Reordering
> **Current impl (Nov 19)**: `ValidatorBatcher` fully implements directed dependency graph construction (Reader->Writer edges) and cycle detection/breaking.
- [x] Extend OCC validator to collect `Batch` objects (configurable `validator_batching`, `batch_size`, `max_wait_us`) via `ValidatorBatcher`.
- [x] Build dependency graph per batch: nodes = transactions, edges = read-write dependencies (Reader->Writer) captured via `RequestAccessTracker`.
- [x] Implement cycle detection and breaking algorithms:
  - [x] Directed DFS cycle detection.
  - [x] Victim selection based on policy (currently max-degree/min-abort).
- [x] Add selective abort responses for victim transactions.
- [ ] Support policy hooks (currently `min_abort` heuristic; other policies map to score-based selection).
- [ ] Expose policy selection + weights via JSON config.
- [x] Ensure graph construction & reordering uses RustyCpp-safe containers.
- [x] Emit metrics: batch size, reorder latency, FVS density, and drop counts via `trcc_*` counters.

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
- [x] Add CLI flag `--txn-config=/path/to/json`. If absent, fall back to defaults (feature off).
- [ ] Validate JSON at startup; fail fast with descriptive error.

### 5. RustyCpp & Safety
- [ ] Use RustyCpp smart pointers for new data structures (graphs, batches).  
- [ ] Annotate unsafe sections with `// @unsafe` and justify.  
- [ ] Add unit tests for borrow-checker (e.g., `make borrow_check_all_dbtest`).

### 6. Telemetry & Logging
- [x] Integrate with existing stats (event counters surfaced via stats server + `results/trcc_metrics.json`).  
- [x] Log reordering decisions when `MAKO_LOG_LEVEL=debug`.  
- [x] Export JSON snapshots summarizing TRCC counters for offline analysis (`results/trcc_metrics.json`).

### 7. Documentation
- [ ] Cross-link config options in `doc/config.md`.  
- [ ] Update `commands.md` / `run.md` with new CLI flag usage.  
- [ ] Add section to `doc/mako-eocc-design.md` describing the module.

### 8. Validation Plan
- [x] Unit tests for graph builder & policies (synthetic workloads).  
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
1. Implement sort-based + policy-aware FVS heuristics (age/tail/thread).  
2. Validate via scripted experiments + integrate findings into paper/results.  
3. Harden config schema validation + docs cross-links.  
4. Stand up experiments from `doc/experiments.md`.  
5. Iterate on performance + memory profiling.
