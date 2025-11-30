## OCC Code Inventory

This note catalogs every place in the tree (outside of `src/deptran/occ/`) that participates in OCC execution, batching, validation, or instrumentation. Use it as the master map when wiring new features or experiments.

### 1. Mako Single-Node OCC Path (`src/mako`)

- `txn.h`: Defines `transaction_base` (state machine, abort reasons, read/write/absent sets) and the templated `transaction<Protocol, Traits>`. Batch validation hooks rely on the friend declaration for `mako::BatchValidator`. Also owns event counters and data structures (`dbtuple_write_info`, etc.) used during commit/validation.
- `txn_impl.h`: Implements `transaction<>::commit()`, which performs write locking, read validation, absent-set validation, and write install. When `ENABLE_BATCH_VALIDATION` is defined it lazy-initializes the `BatchValidator`, reads env vars `MAKO_ENABLE_BATCH_VALIDATION`, `MAKO_BATCH_VALIDATION_SIZE`, `MAKO_BATCH_VALIDATION_MAX_WAIT_US`, and decides whether to skip individual validation based on batch results. This is the critical hook for Task 7/10–13.
- `txn_occ_batch_validation.h`: Parallel batch validator template. Collects transactions, enforces batch-size/time thresholds, calls `ValidateTransactionReadSet()` in parallel (OpenMP if available), updates stats counters, and marks txn state/abort reason. Runtime-configurable through env vars and compile-time `ENABLE_BATCH_VALIDATION`/`_OPENMP`.
- `txn_proto2_impl.{h,cc}` (and related logging files): Manage per-core logging, persistence epochs, and speculative replay. Important when tying OCC optimizations back to Mako’s per-core log design (from the OSDI paper) and when enabling speculative replication experiments.
- `masstree_*`, `txn_btree.*`, `tuple.*`: Provide the Masstree-backed storage engine, versioned tuples, and B-tree access patterns that OCC validates against. When tuning OCC we often touch `dbtuple::lock`, `VersionedRow`, etc.
- `benchmarks/`:
  - `bench.cc`, `bench.h`, `abstract_db.*`: Define benchmark harness and DB abstraction that wrap the OCC transactions.
  - `dbtest.cc`: Parses command line flags (`--num-threads`, `--shard-config`, `--site-name`, etc.), loads transport configs, initializes shards (single or multi-shard), and launches benchmark workers. Any automated experiment runner eventually invokes this binary.
  - `tpcc.cc`, `ycsb.cc`, etc.: Provide concrete workloads whose contention parameters we’ll vary (threads per shard, warehouses, Zipf theta, etc.).
- `ci/ci.sh`: Enumerates supported experiment presets (`simpleTransaction`, `shard1Replication`, `shardFaultTolerance`, etc.). Useful for scripting baseline vs. optimized runs on constrained hardware.

### 2. MemDB OCC Core (`src/memdb`)

- `txn_occ.cc`: Implements OCC transaction semantics that the deptran scheduler invokes. Handles version tracking per row/column (`ver_check_read_`, `ver_check_write_`), reference counting, version verification, and cleanup. When enabling new instrumentation or reordering algorithms in deptran mode we need corresponding changes here.
- `row.h`, `table.h`, `versioned_row.*`: Provide the `VersionedRow` API used by both deptran OCC and Mako’s single-node OCC path for version checks and locking.

### 3. Deptran OCC Scheduler (`src/deptran/occ`)

*(Kept for completeness even though the task called out “beyond `src/deptran/occ`”; the scheduler bridges deptran workloads into the MemDB + instrumentation stack.)*

- `scheduler.{h,cc}`: Creates `SchedulerOcc`, wires OCC-specific `DoPrepare()` to run version checks, record abort reasons, and (when `OCC_ANALYSIS_ENABLED`) emit profiling data. Also defines `printAnalysisStatistics()` which the documentation instructs us to call on shutdown.
- `tx.{h,cc}`, `coordinator.h`: Wrap OCC transactions in deptran’s `Tx` abstractions.
- `occ_analysis.{h,cc}`, `scheduler_instrumented.cc`: Contain the performance/false-abort tracking helpers that docs like `ANALYSIS_INTEGRATION_GUIDE.md` reference. Guards via `#ifdef OCC_ANALYSIS_ENABLED`.

### 4. Transport, Replication, and Speculation Hooks (`src/mako/lib`, `doc/*.md`)

- `src/mako/lib/transport_*.{cc,h}`: Runtime selection between RRR and eRPC transports (also described in `doc/transport_backends.md`). Experiment scripts may need to toggle `MAKO_TRANSPORT` or related env vars when measuring OCC changes over different transports.
- `src/mako/lib/server.cc`, `shardClient.*`, `multi_transport_manager.*`: Manage shard leader/follower lifecycles, including speculative commit/replication flows from the OSDI25 paper.
- Documentation under `doc/transport_*`, `doc/rrr-*.md`, `doc/table-allocation.md`, etc., explains how transport and storage tuning interacts with OCC performance. These inform configuration choices when we lack the paper’s large cluster.

### 5. Benchmark & Experiment Orchestration

- `scripts/` & `ci/ci.sh`: Provide higher-level experiment runners (e.g., `./ci/ci.sh simpleTransaction`) that set up configs, spawn dbtest, and capture stats.
- `run_performance_test.sh`, `test_batch_validation_stress`, `test_simple_batch_validation.sh`: Already-built scripts/binaries to compare sequential vs. batch validation and to sanity-check feature flags. They read the same env vars the docs mention.
- `BenchmarkConfig` (in `benchmarks/benchmark_config.h`): Holds runtime parameters (thread counts, shard placement, Paxos roles) that determine how OCC traffic is generated.

### 6. Feature Flags & Build Plumbing

- `CMakeLists.txt`: Adds `ENABLE_BATCH_VALIDATION`, `ENABLE_OPENMP`, `OCC_ANALYSIS_ENABLED`, and ties additional sources (`txn_occ_batch_validation.h`, `occ_analysis.cc`) into builds when requested.
- Environment variables used at runtime:
  - `MAKO_ENABLE_BATCH_VALIDATION`, `MAKO_BATCH_VALIDATION_SIZE`, `MAKO_BATCH_VALIDATION_MAX_WAIT_US` – batch validator controls.
  - `MAKO_TRANSPORT`, `MAKO_TRANSPORT_THREADS`, etc. – transport selection and thread counts.
  - Deptran configs (YAML under `config/`) – pick OCC mode via `tx_proto: MODE_OCC`.

### 7. Outstanding Mapping Tasks

1. Finish reading the remainder of `OCC_ENHANCEMENT_PLAN.md` to capture proposed modules like `dependency_graph.cc`, `fvs_finder.cc`, and planned scripts (> L400).
2. Trace where `txn_proto2_impl` feeds commit logs into replication so future transaction reordering work stays consistent with replay requirements.
3. Inventory how benchmark workers pull in OCC transactions (e.g., `bench_runner`, `abstract_db`) to know where to plug in new telemetry or control knobs.

This inventory should be updated whenever we discover new OCC touchpoints (e.g., additional scripts, RustyCpp annotations, or config files) so we always know which components must be modified/tested when rolling out OCC optimizations.

