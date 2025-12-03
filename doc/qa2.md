## OCC Implementation in Mako

### 1. What OCC code is just a simulation?

The file `src/mako/vec/occ.cpp` is a **standalone microbenchmark/simulator**:
- It defines its own `Transaction` struct with `readSet` and `writeSet`.
- It uses integer `lock` and `version` fields with CAS and artificial `sleep_for` calls to model latency.
- It is **not used** in the main Mako transaction path.

You can treat this file as an experiment, not the production OCC engine.

---

### 2. The real OCC used by Mako’s storage engine

For the main Mako path (what you hit when running `dbtest` with `mbta_wrapper`), the OCC implementation comes from the **STO/Silo-style transaction engine**:

- Per-record **transaction ID (TID)**:
  - Type: `TransactionTid::type` (a 64-bit integer) in `benchmarks/sto/Interface.hh`.
  - Encodes:
    - `threadid_mask` – which worker thread holds the lock.
    - `lock_bit` – whether the record is locked.
    - `increment_value` – how much to add per commit to create a new version.
- Per-transaction **read set** and **write set**:
  - Implemented inside `Transaction` in `benchmarks/sto/Transaction.hh` / `Transaction.cc`.
  - On read, the transaction remembers the tuple pointer and the observed TID.
  - On write, the transaction buffers a new value in its write set (no in-place write until commit).
- **Commit-time validation (Silo-style OCC)**:
  1. Lock all write-set tuples using `TransactionTid::try_lock/lock_write`.
  2. Re-check that all read-set tuples still have the same TID they had when first read.
  3. If validation passes, apply writes and bump TIDs with `TransactionTid::inc_write_version`.
  4. Release locks; if anything fails, abort and release locks instead.

This matches the paper’s statement that Mako “extends Silo’s single-node optimistic concurrency control (OCC) protocol to a distributed variant” (§4.2).

---

### 3. Where is OCC implemented in the code?

#### 3.1 Record TIDs and locking – `TransactionTid`

- **File**: `src/mako/benchmarks/sto/Interface.hh`
- Defines the layout and operations on each record’s TID:
  - `threadid_mask`, `lock_bit`, `increment_value`.
  - `try_lock`, `lock_read`, `lock_write`, `unlock_read`, `unlock_write`.
  - `inc_write_version` for bumping the version on commit.

Every Masstree record stored by Mako carries one of these TIDs; they are the foundation of OCC.

#### 3.2 Transaction object – `Transaction`

- **Files**:
  - `src/mako/benchmarks/sto/Transaction.hh`
  - `src/mako/benchmarks/sto/Transaction.cc`
- Responsibilities:
  - Track **read set** and **write set** for the current transaction (`TransItem` entries).
  - On read:
    - Attach a read item to the current `Transaction`.
    - Record the observed TID so it can be validated later.
  - On write:
    - Attach a write item that remembers the key, new value, and the target tuple.
  - On commit:
    - Acquire locks on the write set (via `TransactionTid` helpers).
    - Validate the entire read set (opacity / version checks).
    - Apply writes and bump TIDs.
    - Release locks and clear the transaction’s internal state.

This is the **core OCC algorithm**—very close to Silo’s.

#### 3.3 Masstree integration – `MassTrans`

- **File**: `src/mako/benchmarks/sto/MassTrans.hh`
- Purpose:
  - Bridge between STO’s `Transaction` and Masstree’s `versioned_value` nodes.
  - Provide methods like `transGet`, `transPut`, `transInsert`, and `transDelete`.
- On `transGet`:
  - Navigate Masstree using an unlocked cursor.
  - If a record is found:
    - Attach a read-only item to the current transaction.
    - Use `atomicRead` to read the value and its version.
    - Record the version into the transaction’s read set for later validation.
- On writes (`trans_write` variants):
  - Look up or insert a Masstree entry.
  - Attach a write item to the transaction.
  - Stage the new value (no in-place write until commit).

So **Masstree is the data structure**, and `Transaction`+`TransactionTid`+`MassTrans` implement OCC over it.

#### 3.4 Mako’s adapter – `mbta_wrapper`

- **Files**:
  - `src/mako/benchmarks/mbta_wrapper_arena.hh`
  - `src/mako/benchmarks/mbta_wrapper.hh` / `_norm.hh`
- These classes adapt STO to the generic `abstract_db` interface that benchmarks use:
  - `mbta_wrapper::new_txn` calls `Sto::start_transaction();`.
  - `mbta_wrapper::commit_txn` calls `Sto::commit();` and wraps STO aborts as `abstract_db::abstract_abort_exception`.
  - `mbta_wrapper::abort_txn` calls `Sto::abort();`.
  - `mbta_ordered_index::get/put/insert/remove` delegate to a `MassTrans` instance (`mbta`) for all actual Masstree work.

When you run `dbtest`, the call stack looks like:

```text
benchmark code
  → abstract_db (interface)
    → mbta_wrapper
      → STO Transaction + MassTrans
        → Masstree
```

---

### 4. Other OCC code paths (not used by Mako’s mbta path)

There are **two other OCC implementations** in the tree:

1. **Deptran/Janus OCC**:
   - Files:
     - `src/memdb/txn_occ.h` (`mdb::TxnOCC`)
     - `src/deptran/occ/scheduler.cc` (`SchedulerOcc`)
   - Used when running Deptran protocols via `s_main`, not when using Mako’s `mbta_wrapper`/Masstree engine.

2. **vec-based simulation**:
   - File: `src/mako/vec/occ.cpp`
   - A standalone microbenchmark/simulator; not part of the production OCC engine.

---

### 5. Summary

- The **OCC implementation that matters** for Mako’s main code path is STO’s Silo-style OCC:
  - Per-record `TransactionTid` with lock + version.
  - Per-transaction read/write sets in `Transaction`.
  - Commit-time validation and version bumping in `Transaction::stop` and related logic.
  - Masstree integration via `MassTrans`.
  - Exposed to the rest of the system through `mbta_wrapper` and `abstract_db`.
- The code in `src/mako/vec/occ.cpp` is **just a simulation** and is not used in the real transaction engine.


