## Transaction Reordering Design (Validator Phase)

This document sketches the implementation plan for feature **11** in the task list: “Clean and minimal implementation of the different txn reordering algos from OCC paper.” It translates Ding et al. (PVLDB’18) plus the repo’s `OCC_ENHANCEMENT_PLAN.md` into concrete components, flags, and integration points.

### 1. Goals
1. Batch transactions before validation (already available via `BatchValidator`); plug a reordering stage into the batch lifecycle.
2. Build a dependency graph over each batch (nodes = txns, edges = read→write conflicts).
3. Apply FVS-based algorithms (SCC greedy, sort-based greedy, hybrid with brute force for tiny SCCs). Support policies (prod-degree, priority, abort count, thread-aware heuristics).
4. Abort the chosen feedback set, then validate the remaining transactions in topological order.
5. Keep the entire feature flag-protected and default-off (`MAKO_ENABLE_TXN_REORDER=0`).

### 2. High-Level Architecture

```
BatchValidator::validate_batch_parallel()
  └─ if txn_reorder_enabled:
        batch = collect_txn_metadata()
        graph = DependencyGraphBuilder::build(batch)
        fvs   = FVSFinder::compute(graph, policy)
        abort(fvs)
        order = graph.topo_sort_without(fvs)
        validate(order)
     else:
        validate original order
```

Key components:
- `DependencyGraphBuilder`: builds adjacency lists and auxiliary structures (write map / read map). Designed for reuse by both sequential and parallel graph backends.
- `FVSFinder`: pluggable algorithms. Each returns a set of txn IDs to abort plus (optionally) statistics (cycles removed, policy score).
- `TxnReorderController`: Orchestrates builder + FVS finder, applies policies, and returns reordered transaction lists to the batch validator.

### 3. Core Data Structures

#### 3.1 `TxnBatchMetadata`
```cpp
struct TxnBatchMetadata {
  size_t batch_id;
  std::vector<TxnDescriptor> txns;
};

struct TxnDescriptor {
  txnid_t id;
  txn_type* txn;
  std::vector<KeyHash> read_keys;
  std::vector<KeyHash> write_keys;
  double priority; // e.g., latency weight, retry count
};
```
- Key extraction uses helpers similar to OCC analysis instrumentation (read/write sets already stored in `txn_impl`).
- `KeyHash` = table name + key hash to avoid heavy string copies.

#### 3.2 `DependencyGraph`
```cpp
class DependencyGraph {
 public:
  void add_node(txnid_t id, double priority);
  void add_edge(txnid_t from, txnid_t to);

  size_t node_count() const;
  size_t edge_count() const;
  const std::vector<txnid_t>& outgoing(txnid_t id) const;
  std::vector<txnid_t> topo_sort_without(const std::unordered_set<txnid_t>& remove) const;
  std::vector<std::vector<txnid_t>> strongly_connected_components() const;
};
```
- Backed by contiguous vectors for cache locality; node IDs remapped to dense indices for efficiency.
- When `MAKO_GRAPH_VGC=1`, we’ll swap in the parallel/VGC backend for SCCs (Section 4).

### 4. Parallel Graph Backend (Task 15)

Borrow from the SCC paper:
- Implement `VgcReachability` to perform multi-source reachability without O(D) synchronization. Each thread explores deeper local search, using the “vertical granularity control” strategy.
- Use a parallel hash bag to collect next-frontier vertices; dynamic resizing avoids rehash overhead.
- Integration: use sequential graph builder for small batches; for batches ≥ threshold, pass adjacency data to `ParallelDependencyGraph`, which exposes the same API but runs SCC detection and edge trimming using VGC.
- Config flags:
  - `MAKO_GRAPH_VGC=1` enables the parallel backend.
  - `MAKO_GRAPH_THREADS` controls worker threads (default = min(batch size, hardware_concurrency)).

### 5. Algorithms (FVSFinder)

#### 5.1 SCC-Based Greedy
1. Compute SCCs (Tarjan for sequential backend, VGC-based for parallel backend).
2. For each SCC with >1 node, remove the vertex ranked highest by policy.
3. Recurse until all SCCs are size 1.

#### 5.2 Sort-Based Greedy
1. Rank all nodes by policy.
2. Iteratively remove the top-ranked node, trim graph, re-rank (optionally every k iterations).
3. Lightweight, suited for large batches.

#### 5.3 Hybrid
1. For SCCs with ≤ threshold nodes, run brute-force search or branch-and-bound to find minimal FVS.
2. For larger SCCs, fall back to SCC greedy.

Policies (pluggable):
- `prod_degree` (default from Ding et al.)
- `priority_weighted` (higher app priority kept if possible)
- `min_abort_count` (prefer aborting transactions with many retries)
- `thread_affinity` (spread removals across worker threads to improve CPU balance)

### 6. Integration with Batch Validator

1. Add new flag: `MAKO_ENABLE_TXN_REORDER`.
2. Extend `BatchValidator::validate_batch_parallel`:
   - After collecting batch transactions, call `TxnReorderController::reorder(batch, config)` if flag set.
   - Controller returns a vector of `txn_type*` in new validation order plus list of explicit aborts.
   - Apply aborts immediately (`txn->state = TXN_ABRT` + reason `BATCH_REORDER_ABORT`).
   - Validate remaining transactions sequentially (or still in parallel but respecting order). Option: parallelize validation by slicing order but ensuring dependencies enforced (e.g., using dependency levels).
3. Export knobs through env vars (default-off) and YAML:
```yaml
occ:
  txn_reordering:
    enabled: true
    algorithm: scc_greedy
    policy: prod_degree
    hybrid_threshold: 5
    parallel_graph: true
    graph_threads: 4
```
- Map YAML to runtime config object (mirroring `OCC_ENHANCEMENT_PLAN.md` Section 5.1/5.2).

### 7. Observability & Analysis Hooks
- Emit counters: `g_evt_txn_reorder_batches`, `g_evt_txn_reorder_aborts`, `g_evt_txn_reorder_saved_aborts`, `g_evt_txn_reorder_graph_build_us`, etc.
- For troubleshooting, log summary per batch when `MAKO_LOG_LEVEL=debug`: batch size, algorithm, aborted txns, top cycle info.
- Record metadata in experiment results (extend `run_occ_suite.py` parser to capture `agg_throughput_tps` deltas for reordering scenarios once implemented).

### 8. Safety & Performance Considerations
- Batch metadata extraction must avoid copying entire rows; rely on existing read/write set structures.
- Concurrency: graph building occurs before validation while the batch is isolated, so no cross-thread races if we keep operations within the batch.
- Memory: reuse graph buffers with `std::vector::reserve` to avoid per-batch allocations.
- Ensure ordering respects OCC semantics: commit order produced by topological sort ensures no read-after-write dependencies are violated.

### 9. Implementation Steps
1. Implement `TxnBatchMetadata` helper (shared by analysis tooling). Add unit tests for set extraction.
2. Build sequential `DependencyGraph` + builder class with tests (synthetic conflict data).
3. Implement FVS algorithms + policies; verify against sample graphs (compare with brute force for small batches).
4. Integrate into `BatchValidator` behind feature flag. Add metrics/logs.
5. Introduce parallel backend (optional flag) and integrate VGC/hash bag.
6. Update automation (`run_occ_suite.py`) to run new scenarios (E2/E3 from experiment plan) once code path exists.

### 10. Experiment Plan Hook
- For E2 (“only txn reordering”), enable `MAKO_ENABLE_TXN_REORDER=1` and disable batch validation to isolate effect.
- For E3 (“parallel graph”), additionally enable `MAKO_GRAPH_VGC=1` and tune `MAKO_GRAPH_THREADS`.
- Ensure summary CSV captures new metrics so we can compare throughput/abort deltas.

This design keeps the reordering logic modular, aligns with Ding et al.’s algorithms, and stays consistent with the repo’s existing batch validation infrastructure and feature-flag philosophy. Next steps: start coding the metadata builder + dependency graph modules under `src/mako/occ_reorder/` (or similar), following the steps outlined above.

