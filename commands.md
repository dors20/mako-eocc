# Mako-eocc Command Reference

This cheat-sheet aggregates the commands you need to build, test, benchmark, and operate the geo-replicated Mako runtime. Commands assume you are at the repository root (`/home/md/fall25/cse532/project/mako-eocc`).

---

## 1. Environment Preparation

```bash
# Install apt dependencies (Ubuntu 22.04 / Debian 12)
bash apt_packages.sh

# Install Rust toolchain required by RustyCpp helpers
source install_rustc.sh

# Initialize submodules if the clone was shallow
git submodule update --init --recursive
```

Optional tooling:

```bash
# Install Python packages needed for plotting + scripts
pip install -r requirements.txt
```

---

## 2. Build Targets

### CMake (default)

```bash
# Configure + build everything (auto-detects core count, defaults to 4 parallel jobs)
make -j32

# Rebuild from scratch
make clean
make -j32
```

### Direct CMake invocation

```bash
cmake -S . -B build
cmake --build build --parallel 32
```

### Legacy makefile helpers

```bash
# Partial build targets (dbtest, unit tests, Paxos microbench)
cmake --build build --target dbtest --parallel 32
cmake --build build --target paxos_async_commit_test --parallel 32
```

---

## 3. Core Test Suites

Use `ci/ci.sh` for repeatable scenarios; it compiles first if needed.

```bash
# Compile only
./ci/ci.sh compile

# Simple single-shard transaction smoke test
./ci/ci.sh simpleTransaction

# Simple Paxos replication with default transport
./ci/ci.sh simplePaxos

# Two shards, no replication (rrr transport)
./ci/ci.sh shardNoReplication

# One shard with replication (speculative 2PC + Paxos)
./ci/ci.sh shard1Replication

# Two shards with replication
./ci/ci.sh shard2Replication

# RocksDB persistence + queue regression tests
./ci/ci.sh rocksdbTests

# Fault-tolerance / shard reboot loop
./ci/ci.sh shardFaultTolerance

# Multi-shard single-process mode (MultiTransportManager)
./ci/ci.sh multiShardSingleProcess

# Full CI battery
./ci/ci.sh all
```

Unit / integration tests through `ctest`:

```bash
cd build
ctest --output-on-failure        # serial
ctest -j16 --output-on-failure   # parallel
```

---

## 4. Running `dbtest` Manually

Single shard (no replication):

```bash
./build/dbtest \
    --shard-config config/mako_single_node.yml \
    --shard-index 0 \
    --num-threads 8
```

Multi-shard single-process mode (shards 0 and 1, 4 threads total):

```bash
./build/dbtest \
    --local-shards=0,1 \
    --shard-config src/mako/config/local-shards2-warehouses4.yml \
    --num-threads 4
```

Switch transports at runtime:

```bash
MAKO_TRANSPORT=erpc ./build/dbtest config/mako_new_format.yml
```

---

## 5. `run.py` and Batch Experiments

Run a single distributed trial (combining multiple config fragments):

```bash
./run.py -f config/1c1s1p.yml \
         -f config/tpcc.yml \
         -f config/client_closed.yml \
         -f config/rw.yml
```

Batch sweeps using `run_all.py` (auto-generates per-scenario configs):

```bash
./run_all.py \
    -hh config/hosts-local.yml \
    -s '1:4:1' \
    -c '1:3:1' \
    -r '3' \
    -cc config/rw.yml \
    -cc config/client_closed.yml \
    -cc config/brq.yml \
    -b rw \
    -m brq:brq \
    --allow-client-overlap test_run
```

AWS/EC2 helper scripts live in `scripts/aws` and `pylib/cluster.py`—use those when provisioning remote clusters.

---

## 6. Automated Experiment Runner (`run_experiment.py`)

This helper compiles, launches `bash/shard.sh` on remote roles, and captures logs.

```bash
# Dry-run (generate script only)
python3 run_experiment.py \
    --shards 3 \
    --threads 12 \
    --replicated \
    --micro \
    --dry-run

# Compile + run TPCC-style workload with replication
python3 run_experiment.py \
    --shards 5 \
    --threads 16 \
    --replicated \
    --ssh-user ubuntu

# Cleanup remote processes & remove logs
python3 run_experiment.py --shards 5 --replicated --cleanup-only
```

Flags of interest:

- `--skip-compile` – reuse an existing build (faster iterative runs).
- `--only-compile` – build artifacts without launching experiments.
- `--no-sshpass` – use native SSH agent instead of `sshpass`.

Generated scripts are stored as `experiment_s{shards}_{repl}_{workload}_t{threads}.sh`.

---

## 7. Benchmark-Specific Commands

### Microbench / TPCC variants

```bash
# TPCC with replication (per config examples)
bash examples/test_2shard_replication.sh

# TPCC without replication (RRR transport)
bash examples/test_2shard_no_replication.sh

# Simple Paxos + transaction workload
bash examples/simplePaxos.sh
```

### Paxos microbenchmarks

```bash
# Multi-process Paxos (3 replicas, OCC workload)
python3 scripts/paxos-microbench.py \
    -d 60 \
    -f config/1c1s3r3p.yml \
    -f config/occ_paxos.yml \
    -t 30 \
    -T 100000 \
    -n 32 \
    -P p3 -P p2 -P localhost

# Multi-thread (single process, multi-core)
python3 scripts/paxos-microbench.py \
    -d 60 \
    -f config/1c1s3r1p.yml \
    -f config/occ_paxos.yml \
    -t 30 \
    -T 100000 \
    -n 32 \
    -P localhost
```

### Client load shape (open vs closed loop)

```bash
# Closed-loop clients (bounded concurrency)
./build/dbtest -f config/client_closed.yml -f config/tpcc.yml ...

# Open-loop clients (Poisson arrivals)
./build/dbtest -f config/client_open.yml -f config/tpcc.yml ...
```

### RocksDB persistence verification

```bash
# Run persistence test (queues + RocksDB WAL)
bash examples/run_rocksdb_test.sh

# Replay recorded RocksDB log
./build/rocksdb_replay_app --input path/to/wal
```

---

## 8. Profiling and Tracing

```bash
# Build with Google perftools instrumentation
./waf configure -p build

# Run a representative workload
./build/deptran_server \
    -f config/3c3s3r1p.yml \
    -f config/brq.yml \
    -f config/tpca.yml \
    -P localhost \
    -d 60

# Generate CPU profile
./scripts/pprof --pdf ./build/deptran_server process-localhost.prof > cpu.pdf
```

Enable verbose logging / additional stats:

```bash
export MAKO_LOG_LEVEL=debug
MAKO_TRANSPORT=rrr ./build/dbtest config/mako_new_format.yml 2>&1 | tee logs/mako-debug.log
```

---

## 9. Cleanup & Maintenance

```bash
# Kill lingering test processes
pkill -9 -f dbtest
pkill -9 -f simpleTransaction
pkill -9 -f simplePaxos

# Remove log artifacts
rm -rf results/*.log
rm -rf nfs_*

# Reset build + Masstree perf outputs
make clean
rm -rf out-perf.masstree/* src/mako/out-perf.masstree/*
```

When running remote experiments through `run_experiment.py`, prefer `--cleanup-only` to terminate remote `dbtest` instances across all shard roles.

---

Keep this file updated as you add new CI targets, scripts, or benchmarking flows. Consistent command docs avoid divergence between research scripts and engineering workflows.*** End Patch

