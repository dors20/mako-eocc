## OCC Batching and Reordering – Future Work

This document tracks follow‑up items for the OCC batching / reordering work on the `mako-txn-reordering` branch, especially where we intentionally deviated from or partially implemented Ding et al. (*Improving Optimistic Concurrency Control Through Transaction Batching and Operation Reordering*, PVLDB 2019, `doc/p169-ding.txt`).

### 1. Storage-Layer Reordering (Closer to Ding §3.1)

- **Today**:
  - `StoStorageReorderer` batches *transactions* and uses the generic `StoTxnReorderController` (graph + FVS) to:
    - pre-abort a subset of txns,
    - establish an execution/validation order for survivors,
    - then delegates correctness to the STO engine + `StoBatchValidator`.
  - We do **not** reorder individual read/write operations per key the way Ding’s storage batching does.
- **Future**:
  - Implement per-key storage batches that:
    - buffer read/write requests per key,
    - apply the highest-version writes first (using the versioned store),
    - then serve reads out of the updated versions,
    - and ignore out-of-order lower-version writes, exactly as in §3.1.
  - Integrate this with Mako’s Masstree/RocksDB backends in a way that respects existing version semantics and RocksDB persistence.

### 2. Hybrid FVS Algorithm (Ding §4.1 “Hybrid”)

- **Today**:
  - `FvsSolver` supports:
    - `FvsAlgorithm::BASIC_SCC` – legacy one-victim-per-SCC heuristic.
    - `FvsAlgorithm::SORT_GREEDY` – sort-based greedy with degree-product policy and multi-factor \(k\).
  - `FvsAlgorithm::HYBRID` exists in the config, but currently falls back to `SORT_GREEDY`.
- **Future**:
  - Implement a true hybrid FVS:
    - For SCCs with size ≤ `hybrid_threshold`, run an exact or branch-and-bound search for a minimum (or minimum-weight) FVS.
    - For larger SCCs, fall back to the sort-based greedy algorithm.
  - Add unit tests that compare hybrid vs brute-force on small random graphs for regression safety.

### 3. Advanced FVS Policies (Ding §4.2)

- **Today**:
  - `FvsPolicy` includes:
    - `MIN_ID`
    - `PROD_DEGREE` (in/out-degree product; default when `MAKO_TXN_REORDER_FVS=prod*`).
  - Policy is purely structural plus a static `priority` field in descriptors.
- **Future**:
  - Add policies that incorporate **runtime information**:
    - **Restart-aware**: de-prioritize txns with many restarts (minimize tail latency).
    - **Value-aware**: prioritize high-value txns when workload attaches value/priority.
    - **Thread-aware**: for decentralized architectures, cluster conflicting txns onto the same thread (as in Ding’s thread-aware policy), especially on the STO path.
  - Plumb these signals into `TxnDescriptor` / `StoTxnDescriptor` (e.g., restart count, age, value).
  - Extend env/YAML config so experiments can sweep policies (e.g., `MAKO_TXN_REORDER_POLICY=rdeg`).

### 4. Documentation & Proof Sketches

- **Today**:
  - Correctness arguments live mainly in code comments and external notes.
  - This branch keeps OCC serializability by:
    - preserving the original version-based validation logic,
    - only adding conservative pre-aborts and batch/parallel scheduling.
- **Future**:
  - Add a short `doc/occ_correctness.md` that:
    - states the invariants for the baseline OCC commit path,
    - shows that batch validation + parallelization preserve them,
    - explains why FVS-based reordering cannot admit incorrect commits (only extra aborts),
    - and documents any assumptions (e.g., thread-safe version checks, lock protocol).


