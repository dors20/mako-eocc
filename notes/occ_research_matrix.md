## Research Corpus & Documentation Tracker

This tracker captures key takeaways from the three provided research papers plus the Markdown guidance spread across this repo. It will grow as we digest every doc (including third-party notes where relevant to OCC work). Status legend: ✅ read & summarized, 🟡 skimmed / follow-ups pending, ⏳ queued.

### 1. External Research Papers

| Paper | Core Idea | Mechanisms to Borrow | Experiment Knobs & Metrics | Relevance Notes |
| --- | --- | --- | --- | --- |
| `doc/osdi25-shen-weihai.txt` (Mako OSDI'25) ✅ | Speculative geo-replicated OCC with decoupled replication | Speculative 2PC, vector clocks for bounded cascading aborts, per-core logs | Azure-based TPC-C, shards×threads, WAN RTTs; metrics: throughput, latency CDFs, abort cascades | Baseline definition for Mako; experiment sections (Sections 6–7) describe workloads we must replicate (downsized for 2-core box) |
| `doc/p169-ding.txt` (PVLDB'18 OCC batching) ✅ | Semantic batching across storage & validator to cut OCC aborts | Storage batch reorder, validator batch reorder via dependency graph & FVS heuristics, thread-aware policies | Batch size, policy weights (latency vs throughput), contention knobs; metrics: commit rate, abort breakdown, tail latency | Provides transaction reordering algorithms we must re-implement with feature flags; also defines policies & hyper-parameters |
| `doc/2303.04934v2.txt` (Parallel SCC) 🟡 | Vertical Granularity Control (VGC) & parallel hash bag for reachability/SCC | Local-search reachability, dynamic frontier data structure, work/span analysis | Graph families (social/web/k-NN/lattice), thread count (up to 96 cores), rounds vs BFS; metrics: runtime, work, synchronization rounds | Techniques map to Task 15 (parallel graph algorithms). Need to adapt VGC/hash-bag ideas to OCC dependency graph processing for txn reordering batches |

Next steps: finish deep notes for SCC paper sections 3–6 (VGC internals, hash bag), then translate to OCC dependency graph processing plan.

### 2. Repository Markdown (first-party)

| File | Status | Key Points / Follow-ups |
| --- | --- | --- |
| `ANALYSIS_INTEGRATION_GUIDE.md` ✅ | How to compile/run OCC profiling hooks (flags, helper APIs). Need to ensure new knobs hook into `PerformanceProfiler`/`AbortTracker`. |
| `PARALLEL_BATCH_VALIDATION_PAPER.md` ✅ | Internal whitepaper for current batch validator implementation. Confirms config knobs (`MAKO_BATCH_VALIDATION_*`, OpenMP usage). Validate actual code paths in `src/mako/txn_impl.h`. |
| `OCC_ENHANCEMENT_PLAN.md` ✅ | 16-week roadmap covering analysis → design → implementation. Details YAML/runtime knobs, dependency graph/FVS batching pipeline, evaluation scripts, success criteria. Serves as spec for reordering + parallel algorithms. |
| `OCC_ANALYSIS_README.md`, `TEST_ANALYSIS.md`, `HOW_TO_TEST_OCC_ANALYSIS.md` ✅ | Step-by-step instrumentation/testing workflow. Requires OCC workloads via deptran + explicit shutdown hook for stats dump. |
| `OCC_EXECUTION_PATH.md` ✅ | Control flow from config to `SchedulerOcc::DoPrepare`. Helpful for verifying OCC workloads actually hit our code paths. |
| `IMPLEMENTATION_SUMMARY.md`, `PARALLEL_VALIDATION.md`, `TEST_BATCH_VALIDATION.md`, `PERFORMANCE_ANALYSIS.md` ✅ | Canonical description of batch validator architecture, integration points, config knobs, expected speedups, and test hooks (stress test, stats counters). Needs empirical validation under our workloads. |
| `WHY_NOT_WORKING.md`, `WHY_NO_DIFFERENCE.md`, `WHY_SMALL_IMPROVEMENT_2_CORES.md` ✅ | Troubleshooting notes for OCC analysis + batch validation; document why low-core/short tests show no gain and how to scale experiments. |
| `SUMMARY.md`, `QUICK_TEST.md`, `QUICK_PERFORMANCE_TEST.md`, `PERFORMANCE_TEST_GUIDE.md` ✅ | Quickstart scripts + expectations for baseline vs batch validation; highlight `run_performance_test.sh`, env vars, and interpretation of throughput deltas. |
| `BATCHING_AND_PARALLEL_EXPLAINED.md` ✅ | Narrative walkthrough showing batching + parallel validation interplay, OpenMP requirements, and tuning guidance (batch size/wait). |
| `HOW_TO_RUN_TESTING.md`, `HOW_TO_TEST_PERFORMANCE.md` ✅ | End-to-end testing workflow (functional, perf, stress), env var matrix, sample commands/log parsing, troubleshooting tips. |
| `doc/transport_backends.md` ✅ | Shows how to toggle rrr vs eRPC via env/config/build; outlines perf trade-offs and testing commands. Critical when experiments need to switch transports with limited hardware. |
| `doc/transport_stop_fix.md` ✅ | Explains RRR shutdown race fixes (atomic stop flag, idempotent Stop, request queue signaling). Guides safe scripting for start/stop loops during experiments. |
| `doc/table-allocation.md` ✅ | Describes preallocation of table IDs per shard/follower/learner and legacy behavior. Useful when interpreting benchmark logs or configuring multi-shard workloads on constrained machines. |
| `doc/rrr-rustycpp-migration-plan.md` ✅ | Details phased RustyCpp migration for RRR, enforcing smart-pointer patterns and borrow checking. Reminds us to keep new OCC code RustyCpp-safe (no std::shared_ptr) and to coordinate with ongoing migration. |
| `doc/rrr-rpc.md` ✅ | Full user guide for the RRR RPC stack (IDL, codegen, async patterns). Helpful when wiring OCC experiments that interact with RRR transports or when toggling between rrr/eRPC. |
| `doc/run.md` ✅ | Documents cluster-wide experiment scripts (`run.py`, `run_all.py`). Provides context for future Azure-scale runs when we need automated multi-host scenarios. |
| `doc/*.md` (transport, table allocation, RRR migration, etc.) 🟡 | Skimmed `transport_backends.md`, `transport_stop_fix.md`, `rrr-rpc.md`, `RRR_SAFETY_ROADMAP.md`. Must document implications for OCC experiments (e.g., transport flag combos). |

Planned sequencing: finish first-party root/docs, then sweep third-party submodules for OCC-relevant guidance (e.g., `third-party/rusty-cpp/CLAUDE.md` for coding constraints).

### 3. Coverage Plan

1. Complete deep notes for remaining first-party Markdown (esp. performance/testing guides, batching design docs).  
2. Build an index of OCC touchpoints in source (`src/deptran/occ`, `src/mako`, any scheduler variants) with references back to docs above.  
3. Catalog third-party Markdown only where it affects our implementation constraints (RustyCpp rules already summarized in `CLAUDE.md`; others queued).  
4. Update this tracker after each batch of readings; include links to related TODOs/tests.  

This document will serve as the canonical reference while we implement batching, parallel validation, and transaction reordering experiments under feature flags.

