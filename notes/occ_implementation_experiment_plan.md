## OCC Implementation & Experiment Plan

This plan ties the research/doc sweep to concrete engineering steps so we can deliver the user’s full task list (txn reordering, batching, parallel validation, parallel graph algorithms, curated experiments with profiling) while respecting the current 2‑core / 8 GB machine. It stays flag-protected by default and scales to the larger Azure VM once features look promising.

---

### 1. Baseline & Instrumentation

1. **Confirm Baseline OCC Paths**
   - Use `dbtest` + TPCC/YCSB configs to exercise the single-node OCC path (`src/mako/txn_impl.h`) and the deptran OCC scheduler (`src/deptran/occ/*`) depending on workload.
   - Ensure `OCC_ANALYSIS_ENABLED` instrumentation is compiled in (`cmake -DOCC_ANALYSIS_ENABLED=ON`) so `SchedulerOcc::DoPrepare()` records abort reasons + timing. Add shutdown hook per `ANALYSIS_INTEGRATION_GUIDE.md` to print stats even on short runs.
2. **Batch Validation Baseline**
   - Rebuild with `-DENABLE_BATCH_VALIDATION=ON -DENABLE_OPENMP=ON`. Verify runtime toggles by running `test_batch_validation_stress` with/without `MAKO_ENABLE_BATCH_VALIDATION`.
   - Capture baseline stats on the 2-core host using the “Quick Performance Test” docs (expect little/no improvement; record as reference).
3. **Profiling Hooks**
   - Wrap `transaction<>::commit()` phases with `perf` markers (already partially there) and add CPU profiling scripts:
     - `perf stat -e cycles,instructions,cache-misses -p <dbtest PID>`
     - `perf record -F 99 –g ./build/dbtest ...`
   - On the low-resource box, run shorter 20–30 s windows; on Azure, extend to 60 s+.

---

### 2. Implementation Roadmap (Flag-Protected)

| Phase | Feature | Key Files | Flags / Env |
| --- | --- | --- | --- |
| P1 | **Batch Validator Enhancements** (if needed) | `txn_occ_batch_validation.h`, `txn_impl.h` | `ENABLE_BATCH_VALIDATION`, `MAKO_ENABLE_BATCH_VALIDATION` |
| P2 | **Transaction Reordering (Validator)** | new module under `src/mako/occ_reorder/` (or deptran equivalent), `txn_impl.h` integration | `MAKO_ENABLE_TXN_REORDER=1`, config YAML `occ.transaction_reordering` |
| P3 | **Validator Graph Engine** (dependency graph + FVS) | `dependency_graph.{h,cc}`, `fvs_finder.{h,cc}` per `OCC_ENHANCEMENT_PLAN.md` | `MAKO_REORDER_GRAPH_BACKEND` (“seq”, “vgc”) |
| P4 | **Parallel Graph Algorithms** (VGC + parallel hash bag from `doc/2303.04934v2`) | new `parallel_scc/` helper reused by reordering engine | `MAKO_GRAPH_VGC=1`, `MAKO_GRAPH_THREADS` |
| P5 | **Storage/Operation Reordering + Early Detection** | `txn_impl.h`, `txn.h`, possible `txn_storage_scheduler.cc` | separate env vars mirroring YAML spec (e.g., `MAKO_STORAGE_REORDER`) |

Implementation notes:
- Follow RustyCpp safety rules (no new std::shared_ptr, prefer `rusty::Arc` if needed).
- Each feature remains behind compile-time and runtime toggles; default behavior = existing OCC.
- Shared helper modules (graph builder, hash bag) live under `src/mako/occ_utils/` so both single-node and deptran OCC can reuse them later.

---

### 3. Experiment Storyboard (Tasks 2–14 & 17)

Each experiment logs throughput, latency (P50/P95/P99), abort rate, and CPU profile. On 2 cores we emphasize trends; full-scale runs happen on Azure once results look promising.

#### Workloads & Harness
- **Primary**: TPCC (standard + high-contention variants) via `./ci/ci.sh simpleTransaction` and direct `dbtest --bench tpcc`.
- **Secondary**: YCSB and microbenchmarks for stress tests (`test_batch_validation_stress`, `simpleTransaction`).
- Configs inherit from `config/mako_tpcc.yml`, `config/occ.yml`; add overrides for batch size, reordering policy, etc.

#### Experiment Matrix
| ID | Description | Features Enabled | Hyper-Parameters |
| --- | --- | --- | --- |
| E0 | Baseline OCC | None | threads (2 on local, 8–24 on Azure), warehouses, Zipf θ |
| E1 | Batch Validation Only | `MAKO_ENABLE_BATCH_VALIDATION=1` | batch size ∈ {8,16,32,64}, wait µs ∈ {500,1000,5000} |
| E2 | Txn Reordering Only | `MAKO_ENABLE_TXN_REORDER=1` | FVS policy (prod-degree, priority), batch size, graph backend (“seq”) |
| E3 | Parallel Graph Algorithms | `MAKO_ENABLE_TXN_REORDER=1`, `MAKO_GRAPH_VGC=1` | Graph threads ∈ {2,4,8}, hash bag chunk size, VGC depth |
| E4 | Storage/Operation Reordering Only | `MAKO_STORAGE_REORDER=1`, optional early detection | read/write reorder policy, priority weights |
| E5 | Combined OCC Optimizations | All relevant flags | Mixed hyper-parameter grid to find best combo |

#### Storytelling Goals
1. **Baseline pain**: Show abort rate + CPU usage for E0 (low/high contention).
2. **Batching payoff**: Compare E1 vs E0 on CTN-heavy workloads; highlight CPU profiling (validation time drop).
3. **Reordering impact**: Use Ding et al. policies in E2; show reduced false aborts even before parallel graph enhancements.
4. **Parallel graph gains**: In E3, demonstrate throughput boost once VGC/hash-bag reduce synchronization, referencing SCC paper.
5. **Combined narrative**: E5 replicates paper-style story—throughput + latency improvements, backed by profiling.

Each experiment logs to `results/<timestamp>/<scenario>.json` and writes summary CSVs for easy plotting. Scripts should also capture `perf stat` output and OCC analysis stats for traceability.

---

### 4. Automation & Resource Strategy

1. **Runner Script (`scripts/run_occ_suite.py`)**
   - Reads YAML describing scenarios (features, env vars, configs).
   - Supports `--fast` mode for 2-core box (short runs, fewer threads) and `--full` for Azure.
   - After each run, stores logs + perf data in `results/<scenario>/<host>/`.
2. **CPU Profiling Integration**
   - Optionally wrap commands with `perf stat` via script flag.
   - For long Azure runs, add `perf record` sampling and `flamegraph` generation.
3. **Flag Safety**
   - Automation always sets feature env vars explicitly per scenario to avoid leaking state between runs.
   - Provide `--transport erpc|rrr` to test both backends following `doc/transport_backends.md`.
4. **Limited Hardware Mode**
   - Default thread count = 2, reduced warehouses (e.g., 2) to keep memory under 8 GB.
   - Profile durations ≤ 60 s to avoid overheating/timeouts.
   - Collect aggregated metrics even if throughput differences are small; use them as smoke tests before Azure scaling.

---

### 5. Parallel Graph Algorithm Plan (Task 15 & 16)

1. **Adopt VGC & Parallel Hash Bag**
   - Port the “local search” traversal + multi-level hash bag from the SCC paper into a reusable module (`parallel_scc/vgc_reachability.{h,cc}`).
   - Provide templated interface so validator graph builder can invoke forward/backward reachability on batch dependency graphs.
2. **Thread Allocation Strategy**
   - Introduce `MAKO_GRAPH_THREADS` (default = min(batch size, available cores)). On the 2-core box default to 2.
   - When combined with txn reordering, allow the validator to dedicate `K` threads to graph analysis while validation threads handle read-set checks; we can empirically determine best ratios (Task 16).
3. **Safety & Testing**
   - Add unit tests for VGC traversal and hash bag resizing (ensuring no data races).
   - Gate the entire module behind `MAKO_GRAPH_VGC`; fallback to sequential graph builder when disabled.

---

### 6. Data Collection (Task 17)

For every scenario, collect:
- Throughput (`agg_persist_throughput`, transactions/sec)
- Latency (P50/P95/P99 from benchmark logs)
- Abort metrics (overall + per-reason via OCC Analysis)
- CPU metrics (`perf stat`)
- System utilization (from `/proc/stat`, optional `sar`)

Store results in a structured directory with metadata (config, git SHA, host info). Provide helper notebooks/scripts to visualize throughput vs. abort rate and overlay CPU time distributions to “tell the story.”

---

### 7. Immediate Next Actions

1. Finish doc sweep for any remaining OCC-relevant Markdown.
2. Flesh out the automation script skeleton and sample scenario YAML.
3. Implement smoke-test experiments (E0/E1) on the current machine to validate harness + logging.
4. Begin designing the transaction reordering module (data structures, APIs) per OCC plan.

This plan stays adaptable: we can scale parameters, add scenarios, or adjust feature order as new findings emerge, but the core roadmap keeps every change flag-protected and paired with meaningful experiments and profiling. 

