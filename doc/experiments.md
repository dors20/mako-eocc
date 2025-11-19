## Experiment Plan for Transaction Reordering in Mako

### Objectives
1. Quantify throughput, latency, and abort-rate improvements from storage + validator batching.
2. Explore hyper-parameter space (batch sizes, policies, wait intervals).
3. Compare against baseline Mako and related CC schemes (OCC default, RCC/Janus, 2PL if relevant).
4. Produce publishable plots/tables suitable for a top-tier systems submission.

---

### Baselines
| Name | Description | Notes |
| --- | --- | --- |
| **Mako-OCC** | Current OCC path with no batching/reordering (feature disabled) | Primary control |
| **Mako-OCC (storage only)** | Storage batching enabled, validator off | Isolates storage impact |
| **Mako-OCC (validator only)** | Validator batching/reordering enabled, storage off | Isolates validator impact |
| **Janus/RCC** | Existing deptran protocols | Validate we match/exceed alt protocols under contention |
| **2PL / Tapir** | Optional if time allows | Demonstrates generality |

---

### Workloads
1. **TPC-C** (default mix 45/43/4/4/4)  
   - Warehouse counts: {1, 4, 8, 16}  
   - Thread per shard: {6, 12, 24}.  
2. **Read-Write Micro** (`config/rw.yml`)  
   - Vary read/write ratio: {50/50, 80/20}.  
   - Keyspace size: {1M, 100K} to stress contention.  
3. **Queue / Microbench** (`src/mako/benchmarks/queue.cc`)  
   - Ideal for testing latency/tail distributions.  
4. **Geo Configs** (`config/3c3s1p.yml`, `config/9c3s3r.yml`)  
   - Ensure we test multi-shard + multi-replica scenarios.

---

### Hyper-Parameters to Sweep
| Component | Parameter | Candidate Values |
| --- | --- | --- |
| Storage batching | `storage_batching.enabled` | {true, false} |
|  | `max_batch` | {16, 32, 64, 128} |
|  | `flush_interval_us` | {25, 50, 100} |
|  | `max_batch_bytes` | {64KB, 256KB} |
| Validator batching | `validator_batching.enabled` | {true, false} |
|  | `batch_size` | {16, 32, 64, 128} |
|  | `max_wait_us` | {0, 50, 100} |
|  | `algorithm` | {`scc_greedy`, `sort_greedy`} |
|  | `policy` | {`min_abort`, `age_priority`, `tail_latency`, `thread_aware`} |
| Policy params | `age_weight` | {0.1, 0.2, 0.5} |
|  | `tail_deadline_us` | {500, 1000} |
|  | `thread_penalty` | {0, 1} |

Run factorial combinations for key scenarios; for exhaustive sweeps use Latin hypercube / random sampling to keep runtime manageable.

---

### Metrics to Collect
- Throughput (txn/s) per workload.
- P50/P95/P99 latency.
- Abort rate (overall, intra-batch vs inter-batch).
- Batch stats: average size, reorder time, FVS size (also emitted via `trcc_*` counters).
- TRCC counters: `trcc_storage_direct_dispatch`, `trcc_storage_batches`, `trcc_storage_batch_size`, `trcc_storage_reorder_us`, `trcc_validator_*` (batches, drop counts, reorder latency, conflicts). Machine-readable snapshots land in `results/trcc_metrics.json`.
- CPU utilization per shard & validator.
- Network bytes, replication lag (to verify no regressions).

All metrics should be exported in machine-readable logs (`results/*.json`), and aggregated via `scripts/aggregate_and_graph.sh`.

---

### Experiment Matrix (Example)

| Scenario | Workload | Contention | Feature Config | Expected Outcome |
| --- | --- | --- | --- | --- |
| A1 | TPC-C, 4 shards, 6 threads | Medium | disabled | Baseline reference |
| A2 | Same as A1 | Medium | storage only (max_batch=64) | Fewer stale reads |
| A3 | Same as A1 | Medium | validator only (batch=64, min_abort) | Lower aborts but extra latency |
| A4 | Same as A1 | Medium | both enabled | Combined benefit |
| B1 | RW 100K keys, 24 threads | High | disabled | Stress baseline |
| B2-B5 | Same as B1 | High | vary policies (`min_abort`, `age_priority`, `tail`) | Compare fairness vs throughput |
| C1 | Geo config 3c3s3r | High | best config from tuning | Validate scaling |

Add additional rows as we explore more workloads/policies.

---

### Automation
- Extend `run_experiment.py` to accept `--txn-config JSON`.  
- `dbtest` now accepts `--txn-config=/path/to/config.json`; reuse the YAML-cpp parser so JSON manifests integrate with existing infra.  
- Produce YAML/JSON manifest enumerating experiment runs; store under `results/manifests/txn-reorder/*.json`.  
- Use `scripts/aggregate_run_output.py` to summarize metrics, generate plots for paper.

---

### Expected Deliverables
1. Raw logs for each scenario (shareable tarball).  
2. Summary spreadsheet/CSV with throughput + latency + abort stats.  
3. Plot set: throughput vs contention, latency CDFs, policy comparison bars.  
4. Narrative for paper sections (to feed into `doc/paper/mako-eocc.tex`).

---

### Open Items
- Need to determine validator batch scheduling (timer vs size).  
- Confirm tail-latency metric instrumentation (maybe integrate Perfetto/pprof?).  
- Decide whether to include Janus/RCC results in final paper or keep focus on Mako.

