## OCC Reordering: Status, Correctness, and Remaining Work

This document tracks the current state of the OCC batching / reordering implementation on the `mako-txn-reordering` branch, how it relates to the Ding et al. paper (“Improving Optimistic Concurrency Control Through Transaction Batching and Operation Reordering”), and what remains to be done if we want full feature parity.

---

### Implemented Pieces (High Level)

- **Core OCC batch validation and parallelism**
  - Implemented in `txn_impl.h` and `txn_occ_batch_validation.h`.
  - Batches transactions at commit time and validates read sets and absent sets in parallel.
  - Validation logic is **identical** to the original per-transaction OCC logic, just executed in batches and (optionally) in parallel; this is crucial for correctness.

- **Transaction-level reordering with Ding-style FVS**
  - Graph and metadata:
    - `occ_reorder/txn_batch_metadata.h`
    - `occ_reorder/dependency_graph.h`
  - FVS algorithms and policies:
    - `occ_reorder/fvs_policies.h`
      - `FvsPolicy::{MIN_ID, PROD_DEGREE}`
      - `FvsAlgorithm::{BASIC_SCC, SORT_GREEDY, HYBRID}`
      - `FvsSolver` now supports Ding-style **sort-based greedy FVS** with:
        - Graph trimming (remove nodes that cannot be in cycles).
        - Degree-product policy \((\text{in}+1)\cdot(\text{out}+1)\).
        - Multi-factor \(k\) removals.
  - Backends and controller:
    - `occ_reorder/graph_backend.h` (serial)
    - `occ_reorder/parallel_graph_backend.h` (parallel graph build)
    - `occ_reorder/txn_reorder_controller.h` (generic controller + env-driven options)
  - Integrated in:
    - Core Mako OCC path (via `BatchValidator` in `txn_occ_batch_validation.h`).
    - STO path:
      - Validator-side reordering: `sto_reorder/sto_batch_validator.h`
      - Storage-side reordering: `sto_reorder/sto_storage_reorderer.h`
      - STO metadata/controller: `sto_reorder/sto_txn_batch_metadata.h`, `sto_reorder/sto_txn_reorder_controller.h`.

- **Experiment integration**
  - OCC suite YAML: `config/occ_experiments.yml`.
  - Suite runner: `scripts/run_occ_suite.py`.
  - Analysis / summarization: `scripts/analyze_occ_results.py`, `scripts/summarize_occ_results.py`.
  - Reordering scenarios now explicitly use the Ding-style sort-based greedy FVS via:
    - `MAKO_TXN_REORDER_ALGO=sort`
    - `MAKO_TXN_REORDER_FVS=min|prod`

---

### Correctness Summary

This section summarizes why the current implementation is **semantically safe** relative to the original OCC implementation, even with batching, parallel validation, and reordering enabled.

#### Baseline OCC Semantics

- The original Mako OCC implementation (see `txn_impl.h`) is version-based:
  - Each transaction has:
    - `read_set`: tuples read with their observed version.
    - `write_set`: tuples to be modified, locked in a global order.
    - `absent_set`: B-tree node versions for scans / non-existence checks.
  - Commit flow:
    1. Gather and sort `write_dbtuples`.
    2. Lock write tuples in order and compute a commit TID.
    3. **Validate**:
       - For every read:
         - If tuple is in the write set: `is_latest_version(read_tid)`.
         - Else: `stable_is_latest_version(read_tid)`.
       - For every absent-set entry: compare B-tree version numbers.
    4. If validation passes: apply writes, unlock, and mark committed.
    5. If validation fails: mark aborted, unlock, and clean up.
- This logic is unchanged in the new code; batch validation and reordering are *additive layers* on top.

#### Batch Validation and Parallelism

- `BatchValidator<Protocol, Traits>` batches transactions that reach the commit phase and:
  - Optionally validates them in parallel using `ValidateTransactionReadSet(txn)` for each txn in the batch.
  - `ValidateTransactionReadSet` **reuses the exact same read-set and absent-set logic** as the original commit path; it does not change the semantics of OCC.
- Integration in `txn_impl.h`:
  - After write locks are acquired and `write_dbtuples` is built, the commit path:
    - Attempts to add the txn to a batch (under `ENABLE_BATCH_VALIDATION` + env flags).
    - If the txn is validated in the batch and passes:
      - The commit path **skips the per-txn sequential validation**, because the batch result is equivalent.
    - If the txn fails in the batch:
      - `state` is set to `TXN_ABRT`, and the normal abort path (`do_abort` label) runs.
    - If the txn is not validated in a batch (small batch, timeout, etc.):
      - It falls back to **the original sequential validation code**, unchanged.
- Parallelism safety:
  - The only shared mutable state is per-transaction (`state`, `reason`, local counters).
  - All tuple and B-tree checks are read-only operations with respect to shared data, using the same concurrency invariants as the baseline.
  - Write locks are already acquired before validation starts, just like in the original implementation.

**Conclusion:** Batch validation + parallelism do not weaken OCC guarantees; they either:

- Perform the *same* validation logic in parallel, or
- Fall back to the original sequential validation.

#### Transaction Reordering and FVS

- The **dependency graph** is defined per batch over **transaction descriptors**:
  - Node = transaction.
  - Edges encode constraints derived from read/write sets:
    - Write–write edges: for each key, writers are sorted and edges `writer[i-1] → writer[i]` are added.
    - Read–write edges: for each read, edges `reader → writer` are added for all writers of that key.
  - This matches Ding et al.’s dependency graph definition: edges represent “must precede” relationships for a valid serialization order.
- `FvsSolver` computes a **Feedback Vertex Set (FVS)**:
  - Any computed FVS is **safe**: removing more vertices than necessary only increases the number of aborted transactions; it never allows an unsafe commit.
  - Our FVS implementation does *not* attempt to encode correctness; it is purely an optimization to reduce aborts by:
    - Dropping some transactions pre-emptively (those in the FVS).
    - Trying to produce a better ordering of survivors.
  - **Correctness is still enforced by version checks and locking**, not by the FVS algorithm.
- `GenericTxnReorderController::Plan`:
  - Uses the selected backend (`SerialGraphBackend` or `ParallelGraphBackend`) and `FvsSolver` to produce:
    - `ordered`: survivor transactions in a (partial) topological order.
    - `aborted`: transactions chosen by the FVS.
    - Graph statistics (nodes, edges, components, removed).
  - For **core OCC** (BatchValidator):
    - `aborted` txns are pre-aborted with `ABORT_REASON_USER` and go through the normal abort path.
    - `ordered` txns are validated in that order using the same `ValidateTransactionReadSet` logic.
    - Any txn not in `aborted` or `ordered` is either:
      - Left for per-txn sequential validation, or
      - Ignored by the reordering pass, but still validated normally.
- For **STO** (`sto_reorder`):
  - `StoBatchValidator` and `StoStorageReorderer` use the same controller to:
    - Decide which txns to abort early.
    - Decide the order in which survivors are handed to the underlying STO validation/commit pipeline.
  - The STO engine still enforces its own correctness via versioning and locking; reordering does not bypass these checks.

**Key invariant:** At no point does the graph or FVS logic *skip* or *relax* validation. Reordering may:

- Change the **aborted set** (more or fewer txns commit).
- Change the **order in which survivors are validated**.

But every txn that commits has passed the same OCC version checks and locking protocol as before.

#### Sort-Based Greedy FVS vs Ding et al.

- The `SORT_GREEDY` algorithm in `FvsSolver` mirrors Ding et al.:
  - Maintains an active subgraph of candidate nodes.
  - Iteratively:
    - Trims vertices with indegree or outdegree zero (they cannot participate in cycles).
    - Scores vertices using:
      - Degree-product \((\text{in}+1)\cdot(\text{out}+1)\) when `PROD_DEGREE` is chosen.
      - Priority and id as tie-breakers.
    - Selects the top-k vertices into the FVS.
    - Repeats until the active subgraph is empty.
- Ding et al. use this as a **heuristic to approximate minimal FVS**. Our implementation:
  - Does not rely on optimality for correctness.
  - Is purely about **which txns to abort for better throughput/latency**.

---

### Remaining Work / TODOs (Relative to Ding et al.)

These are tracked here for future work; the current system is safe and usable without them.

- **Storage-layer batching at key granularity (Ding §3.1)**
  - Current status:
    - `StoStorageReorderer` batches *transactions* and uses the graph/FVS layer to reorder/abort them, then delegates to STO.
    - It does **not** implement **per-key** storage reordering: “apply highest-version writes first, then reads on that key,” as in Ding’s storage batching.
  - Possible future work:
    - Introduce a storage-level request buffer keyed by (table, key).
    - Within each batch:
      - Identify validated transactions and their write versions.
      - For each key, apply writes in serialization order before serving reads in the same batch.
    - This would require careful integration with Masstree / RocksDB backends.

- **Hybrid FVS algorithm**
  - Current status:
    - `FvsAlgorithm::HYBRID` is parsed and stored but currently reuses the sort-based greedy implementation internally.
  - Target behaviour (from Ding):
    - For SCCs below a small threshold (e.g. 10–20 nodes), run an exact or branch-and-bound FVS search.
    - For larger SCCs, use the greedy algorithm.
  - This would further reduce aborts but is **not required** for correctness.

- **Advanced reordering policies**
  - Ding et al. discuss policies beyond degree-product:
    - Tail-latency aware (prioritize transactions with many restarts).
    - Transaction value / monetary weight.
    - Thread-aware assignment in decentralized architectures.
  - Current status:
    - Only `MIN_ID` and `PROD_DEGREE` are implemented.
  - Future work:
    - Add a `priority` field and possibly `restart_count` / `value` to descriptor types.
    - Extend `FvsPolicy` and scoring to use these fields.

- **Early conflict detection / operation reordering / hybrid OCC (from `OCC_ENHANCEMENT_PLAN.md`)**
  - These items go beyond Ding et al. and are not currently implemented:
    - Early conflict detection during execution.
    - Intra-transaction operation reordering.
    - Column-level (fine-grained) versioning.
    - Hybrid pessimistic/optimistic control for hot keys.
  - They are orthogonal to the current Ding-style reordering and would need independent design and testing.

---

### Summary

- The current OCC batching + parallel validation + Ding-style sort-based FVS reordering is **correct by construction**:
  - It preserves the original OCC protocol’s version checks and locking semantics.
  - Reordering is a *filter* (pre-aborts + ordering) layered on top of a still-strict OCC validator.
  - Any imperfections in FVS affect only **which** transactions commit, not whether committed transactions are serializable.
- Additional Ding et al. features (key-level storage batching, hybrid FVS, advanced policies) are **nice-to-have optimizations**, not correctness prerequisites, and are listed above as explicit TODOs.


