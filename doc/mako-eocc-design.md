# Mako-eocc Architecture and Module Design

## 1. Context and Goals

Mako targets serializable, geo-replicated transactions that should approach single-node throughput. The OSDI'25 paper highlights three key levers: (1) speculative 2PC that decouples execution from replication, (2) per-core replication pipelines plus vector watermarks to bound cascading aborts, and (3) aggressive multi-core storage using Masstree. The `mako-eocc` fork retains the research codebase while adding automation and documentation for new optimizations.

Design objectives for the fork:

- Preserve research-grade fidelity so optimizations can be validated against the paper’s 3.66M TPS baseline.
- Expose well-documented module seams (execution, replication, networking, benchmarking) for incremental changes.
- Provide reproducible automation (CI, scripts, configs, experiment runner) so contributors can benchmark quickly.

## 2. Deployment Topology

Each operating process starts through `src/deptran/s_main.cc`. At runtime the binary reads YAML configs, launches server workers for each shard-site it owns, optionally boots collocated benchmark clients, and then blocks until both client duration and server shutdown complete. This single entrypoint drives every protocol (Mako OCC, Janus, 2PL, etc.) and allows mixed deployments by swapping the frame implementation.

```134:174:src/deptran/s_main.cc
int main(int argc, char *argv[]) {
  int ret = Config::CreateConfig(argc, argv);
  ...
  auto server_infos = Config::GetConfig()->GetMyServers();
  if (!server_infos.empty()) {
    server_launch_worker(server_infos);
  }
  if (!client_infos.empty()) {
    client_launch_workers(client_infos);
    sleep(Config::GetConfig()->duration_);
    wait_for_clients();
  }
  for (auto& worker : svr_workers_g) {
    worker.WaitForShutdown();
  }
  ...
}
```

**Roles**

| Role | Responsibilities | Implementation hot spots |
| --- | --- | --- |
| Client worker | Generates workload pieces, tracks heartbeats, reports throughput | `src/deptran/client_worker.*`, `ci/ci.sh` |
| Shard leader | Executes OCC transactions, drives speculative 2PC certify step, owns per-core replication logs | `src/mako/benchmarks/dbtest.cc`, `src/mako/lib/server.cc` |
| Shard follower | Replays logs once vector watermark allows, provides failover | `src/mako/lib/server.cc`, Paxos executor |
| Paxos replica | Runs Multi-Paxos per shard/core to replicate log streams | `src/deptran/paxos/*.cc` |


## 3. Execution Pipeline

1. **Scheduling** – `Frame` factories from `src/deptran/frame.cc` instantiate the desired protocol; Mako-specific schedulers run inside the server worker.
2. **Transaction execution** – Each shard runs optimistic transactions against Masstree, keeping compact read/write sets in `transaction_base`.
3. **Certification** – Coordinators run speculative 2PC among shard leaders. Once all shards vote “yes,” the write set is installed locally and becomes visible to later speculative reads.
4. **Replication** – Each leader streams the committed log entry to its Paxos group using per-core `PaxosServer`.
5. **Replay & Acknowledgement** – Followers replay when vector watermarks prove dependency closure; only then do coordinators acknowledge to clients to avoid unbounded cascades.

The ShardReceiver object bridges network RPC and Masstree state by decoding RPC verbs (`get`, `scan`, `validate`, `install`, batched lock/put) and invoking the in-memory engine.

```28:178:src/mako/lib/server.cc
ShardReceiver::ShardReceiver(std::string file) : config(file) { ... }
void ShardReceiver::Register(abstract_db *dbX,
                             const map<int, abstract_ordered_index *> &open_tables_table_idX) {
    db = dbX;
    open_tables_table_id = open_tables_table_idX;
    ...
}
size_t ShardReceiver::ReceiveRequest(uint8_t reqType, char *reqBuf, char *respBuf) {
    switch (reqType) {
    case getReqType:        HandleGetRequest(reqBuf, respBuf, respLen); break;
    case scanReqType:       HandleScanRequest(reqBuf, respBuf, respLen); break;
    case validateReqType:   HandleValidateRequest(reqBuf, respBuf, respLen); break;
    case installReqType:    HandleInstallRequest(reqBuf, respBuf, respLen); break;
    case batchLockReqType:  HandleBatchLockRequest(reqBuf, respBuf, respLen); break;
    ...
    }
    return respLen;
}
```

## 4. Storage Engine and Concurrency Control

`transaction_base` (and its specializations) provide Masstree-aware OCC metadata: read set entries carry tuple pointers + version, write entries keep staging buffers plus concurrent-btree handles, and helper structures (per-core allocators, abort reason counters) expose instrumentation hooks. This is where speculative certification hooks into vector clocks / watermarks.

```50:205:src/mako/txn.h
class transaction_base {
public:
  enum txn_state { TXN_EMBRYO, TXN_ACTIVE, TXN_COMMITED, TXN_ABRT };
  struct read_record_t { const dbtuple *tuple; tid_t t; };
  struct write_record_t {
    enum { FLAGS_INSERT = 0x1, FLAGS_DOWRITE = 0x1 << 1 };
    dbtuple *tuple; const string_type *k; const void *r;
    dbtuple::tuple_writer_t w; marked_ptr<concurrent_btree> btr;
  };
  struct dbtuple_write_info {
    enum { FLAGS_LOCKED = 0x1, FLAGS_INSERT = 0x1 << 1 };
    marked_ptr<dbtuple> tuple;
    write_record_t *entry;
    size_t pos;
  };
  ...
};
```

Takeaways for optimization work:

- Read/write sets are already structured to support validation-time dependency tracking – altering dependency granularity (e.g., compressed vector clocks) should plug into these structures.
- per-core storage macros (`percore<T>`, `percore_lazy<T>`) in `core.h` let new instrumentation stay NUMA-friendly.

## 5. Replication and Vector Watermarks

Replication relies on Multi-Paxos, implemented inside `src/deptran/paxos`. Leaders handle prepare/accept/commit phases, and bulk RPCs (bulk prepare/accept, sync log) enable per-core log shipping. The Paxos scheduler also keeps track of epochs to coordinate failover with the vector watermark recovery steps described in the paper.

```12:75:src/deptran/paxos/server.cc
void PaxosServer::OnPrepare(slotid_t slot_id, ballot_t ballot, ballot_t *max_ballot, const function<void()> &cb) {
  auto instance = GetInstance(slot_id);
  if (instance->max_ballot_seen_ < ballot) {
    instance->max_ballot_seen_ = ballot;
  }
  *max_ballot = instance->max_ballot_seen_;
  n_prepare_++;
  cb();
}
void PaxosServer::OnCommit(const slotid_t slot_id, const ballot_t ballot,
                           shared_ptr<Marshallable> &cmd) {
  auto instance = GetInstance(slot_id);
  instance->committed_cmd_ = cmd;
  if (slot_id > max_committed_slot_) {
    max_committed_slot_ = slot_id;
  }
  for (slotid_t id = max_executed_slot_ + 1; id <= max_committed_slot_; id++) {
    auto next_instance = GetInstance(id);
    if (next_instance->committed_cmd_) {
      app_next_(slot_id,next_instance->committed_cmd_);
      max_executed_slot_++;
      n_commit_++;
    } else {
      break;
    }
  }
  FreeSlots();
}
```

Vector watermarks sit on top of Paxos state to gate replay:

- Each shard maintains per-core log positions (tracked through `sync_util` hooks in `ShardReceiver`), publishes them through gossip, and advances a vector cut only when all dependencies are durable.
- During failures, epochs freeze, the finalized vector watermark (FVW) is computed, and shards roll back speculative windows before resuming new-epoch traffic (per the paper’s algorithm §5). Module-wise this logic spans `src/mako/benchmarks/sto/sync_util.hh`, `src/mako/lib/server.cc`, and Paxos bulk-sync handlers.

## 6. Networking and Transport

Transport backends live in `src/mako/lib`:

- `fasttransport.*` defines the reactor-style event loop that drives request dispatch.
- `transport_backend.h` and `transport_request_handle.h` abstract rrr/rpc vs eRPC (or future transports). Each worker thread only sees `ShardReceiver::ReceiveRequest`.
- `multi_transport_manager.*` (documented in `doc/multi_shard_single_process.md`) allows multiple shards to share one process, each with its own FastTransport instance.

Developers adding new transports only need to implement the backend interface and plug it into `FastTransport`.

## 7. Benchmarking, Workloads, and Automation

- **Benchmarks** – `src/mako/benchmarks` contains TPCC, TPCA, micro workloads, queue tests, RocksDB replay, and helper utilities. Each benchmark registers tables & stored procedures through the `abstract_db` interface so they share the same transaction runtime.
- **Configs** – `config/*.yml` define hosts, shards, clients, concurrency. `NEW_CONFIG_FORMAT.md` explains the modern `sites` + `shard_map` schema used by `bash/shard.sh`.
- **CI/Test harness** – `ci/ci.sh` orchestrates compilation plus scenario scripts such as `simpleTransaction`, `shard1Replication`, `multiShardSingleProcess`, RocksDB persistence smoke tests, and transport-specific suites. This keeps regressions visible without requiring a datacenter.
- **Experiment runner** – `run_experiment.py` codifies end-to-end experiments: compile (with optional skip), launch `bash/shard.sh` for every shard/role via SSH, and capture logs in `results/`. It also supports cleanup-only or dry-run script generation for reproducibility.

```238:311:run_experiment.py
parser.add_argument("--shards", type=int, default=3, help="Number of shards");
parser.add_argument("--replicated", action="store_true", help="Enable replication");
...
runner.experiment_params = { 'shards': args.shards,
                             'is_replicated': args.replicated,
                             'is_micro': args.micro,
                             'threads': args.threads };
...
if args.cleanup_only:
    runner.cleanup(args.shards, args.replicated);
elif args.only_compile:
    runner.compile_project(...);
else:
    if not args.skip_compile:
        runner.compile_project(...);
    runner.run_experiment(args.shards, args.threads, args.replicated, args.micro);
runner.save_commands_script(...);
```

**Operational sequence for a typical benchmark**

1. Provision hosts (local or remote) and fill `bash/shard*.config` with role→host mappings.
2. Run `./ci/ci.sh compile` or `make -j32` for incremental builds.
3. Execute a quick smoke test (`./ci/ci.sh simplePaxos`) before more complex workloads.
4. Launch multi-shard experiments via `run_experiment.py` or `bash/shard.sh` manually, supplying micro/TPCC flags.
5. Collect `results/*.log`, watermark traces, and figure scripts (`scripts/aggregate_and_graph.sh`, `doc/plot.md`).

## 8. Module Inventory

| Area | Directory highlights | Notes |
| --- | --- | --- |
| Transaction runtime | `src/mako/*.cc`, `src/mako/lib/*.cc`, `src/mako/masstree` | Masstree-based storage, RustyCpp migration, persistence glue |
| Protocol toolkit | `src/deptran/` (subdirs `occ`, `janus`, `paxos`, `tapir`, etc.) | Each protocol implements scheduler + coordinator; Mako reuses the OCC frame |
| RPC stack | `src/rrr/` | Custom event-loop RPC for baseline transport; includes coroutine scheduler, reactor, poll manager |
| Benchmarks/tests | `src/mako/benchmarks`, `examples/`, `test/` | Workload definitions, harnesses, sample apps |
| Tooling | `ci/`, `scripts/`, `pylib/`, `bash/` | CI automation, AWS orchestration, data plotting, shard deployment |

## 9. Open Questions / Next Steps

- **Speculation policies** – Evaluate adaptive watermark advancement strategies (e.g., dynamic batching) to reduce the 13 ms batching delay cited in the paper.
- **Failure recovery metrics** – Instrument the vector watermark pipeline (e.g., sync_util logs) to correlate shard downtime with abort cascades.
- **Transport evolution** – Assess eRPC vs rrr transports under multi-shard single-process mode; hooks already exist to set `MAKO_TRANSPORT`.
- **Command coverage** – Keep `commands.md` (in repo root) updated whenever new CI tests or benchmark scripts are added.

This design doc should provide enough scaffolding for future optimizations—engineers can navigate to the right module, understand how it plugs into the speculative pipeline, and extend or replace components with confidence.

