# Mako Q&A: Architecture and Implementation Details

This document provides detailed answers to questions about Mako's architecture, design, and implementation, based on the OSDI'25 paper and codebase.

---

## Table of Contents

1. [What is a Multicore Database?](#q1-what-is-a-multicore-database)
2. [What is Mako?](#q2-what-is-mako)
3. [What is Mako's Architecture?](#q3-what-is-makos-architecture)
4. [Single-Server, 8-Core, No-Sharding Setup: Thread Roles and Transaction Lifecycle](#q4-single-server-8-core-no-sharding-setup-thread-roles-and-transaction-lifecycle)
5. [Entry Points, Deployment Options, and Code Architecture](#q5-entry-points-deployment-options-and-code-architecture)
6. [What is the Current OCC Implementation in Mako and Where Is It Implemented?](#q6-what-is-the-current-occ-implementation-in-mako-and-where-is-it-implemented)

---

## Q1: What is a Multicore Database?

### Definition

A **multicore database** is a database system specifically designed to efficiently utilize modern multi-core processors by running multiple worker threads in parallel, each executing transactions concurrently on different CPU cores. This architecture aims to achieve high throughput by maximizing CPU utilization and minimizing synchronization overhead between cores.

### Key Characteristics (from Paper)

According to the paper (Section 2.1, lines 185-192):

> "In recent years, with the deployment of advanced networking hardware and kernel bypassing techniques, this issue can be mitigated within a datacenter. For example, FaRM [25, 26] equipped with RDMA can reduce the remote access latency and achieve very high per-shard throughput."

The paper references systems like **Silo** (lines 226, 1895-1898) as examples of multi-core transactional databases that can achieve **1.66M transactions per second (TPS)** on a single machine.

### Core Design Principles

1. **Thread-Per-Core Model**: Each CPU core runs one or more dedicated worker threads
2. **Lock-Free or Optimistic Concurrency Control**: Minimizes contention between cores
3. **NUMA-Aware Memory Management**: Allocates memory local to CPU cores
4. **Partitioned Data Structures**: Reduces sharing between cores

### Implementation in Mako

**Worker Thread Management** (`src/mako/thread.cc:21-31`):

```cpp
void ndb_thread::startBind(int core_id)
{
  thd_ = std::move(thread(&ndb_thread::run, this));
  pthread_setname_np(thd_.native_handle(), ("worker_"+std::to_string(core_id)).c_str());
  //cpu_set_t cpuset;
  //CPU_ZERO(&cpuset);
  //CPU_SET(core_id, &cpuset);
  //pthread_setaffinity_np(thd_.native_handle(),sizeof(cpu_set_t), &cpuset);
  if (daemon_)
    thd_.detach();
}
```

**CPU Affinity and Core Binding** (`src/mako/vec/occ.cpp:75-87`):

```cpp
void workerThread(int thread_id, int& local_throughput, int& local_aborts) {
    pthread_t thread = pthread_self();

    cpu_set_t cpuSet;
    CPU_ZERO(&cpuSet);
    int core_id = thread_id % 2? (thread_id / 2) % 64 : (thread_id / 2) % 64 + 64; 
    // Our machine has 128 cores, and (i, i+64) are on the same physical core
    CPU_SET(core_id, &cpuSet);

    // Set CPU affinity for the thread
    if (pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuSet) != 0) {
        perror("pthread_setaffinity_np");
        return;
    }
```

**NUMA-Aware Allocation** (`src/mako/mako.hh:187-194`):

```cpp
// initialize the numa allocator
size_t numa_memory = mako::parse_memory_spec("1G");
if (numa_memory > 0) {
  const size_t maxpercpu = util::iceil(
      numa_memory / benchConfig.getNthreads(), ::allocator::GetHugepageSize());
  numa_memory = maxpercpu * benchConfig.getNthreads();
  ::allocator::Initialize(benchConfig.getNthreads(), maxpercpu);
}
```

### Performance Characteristics

From the paper's evaluation (Section 7.2, lines 1317-1318):

> "Mako is able to scale well in Figure 6a. Mako's throughput at 10 shards (10 servers) is 3.66M transactions per second (TPS)"

**Per-shard performance** (Section 7.9, lines 1882-1898):
- **Silo** (single-machine multicore): **1.66M TPS**
- **Mako** (10 shards distributed): **3.66M TPS total**, ~366K TPS per shard
- The paper notes that Mako preserves **68.4%** of Silo's single-machine throughput after adding sharding and replication

### Why Multicore Matters for Mako

From the paper's introduction (lines 50-53):

> "the throughput that one can achieve with a single in-memory multi-core transactional key-value store is thousands of times higher than what distributed transactional systems typically provide"

Mako builds on multicore database principles (specifically **Silo**'s OCC protocol) to achieve high per-shard throughput, then combines it with speculative execution and geo-replication.

---

## Q2: What is Mako?

### High-Level Definition

From the paper's abstract (lines 23-28):

> "Mako [is] a highly available, high-throughput, and horizontally scalable transactional key-value store. Mako performs strongly consistent geo-replication to maintain availability despite entire datacenter failures, uses multi-core machines for fast serializable transaction processing, and shards data to scale out."

### Key Innovation

The paper's main contribution (lines 28-33):

> "The key innovation in Mako is the use of two-phase commit (2PC) speculatively to allow distributed transactions to proceed without having to wait for their decisions to be replicated, while also preventing unbounded cascading aborts if shards fail prior to the end of replication."

### Three Core Design Goals

From Section 2 (lines 157-160):

1. **Sharded**: Split data across multiple servers for horizontal scalability
2. **Geo-replicated**: Replicate data across datacenters for fault tolerance
3. **High-throughput**: Support millions of transactions per second
4. **In-memory**: Keep data in memory with strong isolation (serializability)

### What Makes Mako Different?

**Decoupling Replication from Transaction Coordination** (lines 84-86):

> "Our observation is that fully decoupling these two components allows the system to perform work speculatively, thereby masking the high overhead introduced by geo-replication."

Traditional systems like **Spanner** (lines 75-78):
- Execute transaction → Certify with 2PC → **Wait for replication** → Return to client
- Replication is **on the critical path**, causing high latency

Mako's approach (lines 231-240):
- Execute transaction → Certify with 2PC speculatively → **Replicate in background** → Continue executing next transaction
- Replication is **off the critical path**, masking geo-replication latency

### Implementation Overview

**Core Data Structure** (`src/mako/mako.hh:102-114`):

```cpp
// Initialize database for a specific shard (multi-shard mode)
// This allows creating isolated database instances for each shard
static abstract_db* initShardDB(int shard_idx, bool is_leader, const std::string& cluster_role) {
  auto& benchConfig = BenchmarkConfig::getInstance();

  Notice("Initializing database for shard %d (cluster: %s, leader: %d)",
         shard_idx, cluster_role.c_str(), is_leader);

  // Create and initialize database instance for this shard
  abstract_db *db = new mbta_wrapper;
  db->init();

  return db;
}
```

### Mako vs. State-of-the-Art

**Performance Comparison** (Section 7.2, Figure 6a):

| System | Architecture | Throughput (10 shards) | Geo-Replication |
|--------|-------------|------------------------|-----------------|
| **Mako** | Speculative 2PC | **3.66M TPS** | ✅ Yes |
| Calvin | Deterministic | 0.43M TPS (8.6× slower) | ✅ Yes |
| 2PC (Spanner-like) | Synchronous | Much lower | ✅ Yes |
| Silo | Single-machine | 1.66M TPS (1 shard) | ❌ No |
| Rolis | Single-shard | Higher than Mako (1 shard) | ✅ Yes, but no sharding |

From lines 1444-1446:

> "Mako at 10 shards is up to 16.7M TPS. Both Mako and OCC+OR can scale well with more shards, but Mako is able to achieve 32.2× higher throughput than OCC+OR at 10 shards"

---

## Q3: What is Mako's Architecture?

### Overview

From the paper (Section 4.1, lines 311-325):

> "A Mako deployment splits its data (key-value pairs) into multiple shards. Each shard is replicated to multiple datacenters with a leader-follower architecture. Similarly to Spanner [20] and other geo-replicated databases, the leaders of different shards ('shard leaders') in Mako can be in different datacenters"

**Visual Architecture** (Figure 1 from paper):

```
┌─────────────────────────────────────────────────────┐
│                     Client                          │
└──────────────────┬──────────────────────────────────┘
                   │ Txn Request
                   ▼
┌─────────────────────────────────────────────────────┐
│           Datacenter 1 (Leaders)                    │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐         │
│  │ Shard 0  │  │ Shard 1  │  │ Shard 2  │         │
│  │ Leader   │  │ Leader   │  │ Leader   │         │
│  │ (24 thds)│  │ (24 thds)│  │ (24 thds)│         │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘         │
│       │             │             │                 │
│  Speculative Execution + Certification (2PC)       │
│       │             │             │                 │
└───────┼─────────────┼─────────────┼─────────────────┘
        │ Paxos       │ Paxos       │ Paxos
        │ Replication │ Replication │ Replication
        │ (async)     │ (async)     │ (async)
        ▼             ▼             ▼
┌─────────────────────────────────────────────────────┐
│           Datacenter 2 (Followers)                  │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐         │
│  │ Shard 0  │  │ Shard 1  │  │ Shard 2  │         │
│  │ Follower │  │ Follower │  │ Follower │         │
│  └──────────┘  └──────────┘  └──────────┘         │
└─────────────────────────────────────────────────────┘
                   │
                   ▼ (After replication complete)
              Reply to Client
```

### Four Main Phases

#### Phase 1: Execution

From the paper (lines 327-332):

> "Execution: A shard leader serves as the coordinator for the transaction execution. The coordinator executes the transaction by optimistically reading from different shard leaders (as needed), and buffering the writes. Reads for some key at a given shard leader return the most recent writes to that key, including those that are certified (explained below) but not yet replicated, but they will not return uncertified writes."

**Key points:**
- **Optimistic reads**: Read latest version without locking
- **Write buffering**: Writes stored in WriteSet, not installed yet
- **Cross-shard reads**: Coordinator contacts other shard leaders as needed
- **Speculative visibility**: Can read certified-but-not-replicated writes

**Implementation** (from Appendix D, lines 2907-2920):

```cpp
// Read from all relevant shards according to keys
// T0 always reads latest version if no failures
tuple<int, vector<int>, ValueType> Read(key, currentEpoch) {
  _epoch, _compVC, _value = masstree[key][0];
  return {_epoch, _compVC, _value};
}

/* *** Execute a transaction T0 in Mako *** */
// Execution phase
{
  // Read() optimistically, and buffer writes
  // ...
}
```

#### Phase 2: Certification (Speculative 2PC)

From the paper (lines 333-343):

> "Certification: After the execution, the coordinator will run a 2PC-based procedure among the shard leaders to confirm that there are no conflicting transactions. The transaction will be assigned a version vector clock. This vector clock represents the serialization order of the transaction. Note that so far no replication has happened, and the transaction is not considered done. Therefore, the transaction 'commit' is speculative, with a chance of rollbacks in case of failures. But after certification, writes are speculatively installed and are ready to be read by later transactions."

**Four Rounds of RPCs** (lines 418-450):

1. **Lock**: Acquire locks on all WriteSet keys across involved shards
2. **GetClock**: Get logical clock from each shard, combine into vector clock
3. **Validate**: Check ReadSet hasn't changed (OCC validation)
4. **Install**: Speculatively install writes with version vector clock

**Detailed Implementation** (Appendix D, lines 2939-2973):

```cpp
// Commit phase
{
  {
    // Phase1: lock keys in writeset
    // ...
  }

  // Get the vector clock for T0
  vector<int> compVC = getClock();

  {
    // Phase2: Validate keys in readset
    // ...
  }

  // Phase3: Install & Release locks
  Install(writeSet, currentEpoch, compVC);
}

void Install(writeSet, currentEpoch, compVC) {
  for (auto k, v : writeSet) {
    { // Execute on all relevant shards
      masstree[k].insert(0, (currentEpoch, compVC, v));
      compSId = getCompSIdBySId(locShardID);
      // Ensure subsequent transactions observe a larger value
      int delta = compVC[compSId] - localCounter;
      if (delta > 0)
        localCounter.fetch_and_add(delta);
    }
  }
}
```

**Vector Clock Design** (lines 407-416):

> "Version vector clock. The version in Mako is a vector clock that consists of n individual logical clocks (c₀, c₁, ..., cₙ₋₁), where n represents the total number of shards in the deployment. Each element represents a logical clock value generated by a shard. The logical clock on each shard increases monotonically."

**Example from paper** (Figure 2, lines 464-471):

```
Shards: S0(a,b,c)  S1(d,e,f)  S2(g,h,i)

T0: W(a)                           → Version: (1,0,0)
T1: R(a); W(d)                     → Version: (1,1,0)  [depends on T0]
T2: R(d); W(d); W(g)              → Version: (1,2,1)  [depends on T1]
T3: W(e)                           → Version: (0,3,0)  [independent]
```

#### Phase 3: Replication (Background Paxos)

From the paper (lines 344-353):

> "Replication: Each shard runs multiple Paxos instances, one for each core (worker thread), to replicate transactions' logs to followers after certification. To maximize throughput, the replication process is completely independent for each shard/core—different shards/cores have zero coordination."

**Per-Core Paxos Streams** (lines 537-560):

> "There are two types of logs in Mako: a transaction log and a per-core replication log (which we call a stream to avoid ambiguity). A transaction log contains the key-value pairs in the WriteSet of the transaction, and its commit version vector clock. In contrast, each entry in a stream corresponds to a batch of transactions (400 in our implementation)."

**Why batching?** (lines 554-559):

> "the reason that each stream entry corresponds to a batch of transactions instead of a single transaction is to eliminate the frequent RPC overhead; all other works in the literature do the same."

**Implementation** (Appendix D, lines 2975-2994):

```cpp
// Replicate transaction logs (batch for the optimization)
// Invoke the callback func once a log is durable
asyncPaxosRep(/* resultant values of transactions */,
             /* compVC */,
             /* threadID */,
             /* locShardID */,
             callbackAsyncPaxosRep);

void callbackAsyncPaxosRep(int threadID, vector<int> compVC, 
                           int locShardID, const string& resultantValue) {
  compSId = getCompSIdBySId(locShardID);
  replicateProgress[threadID] = compVC[compSId];
  localW = MIN(replicateProgress);
  compVW[compSId] = MIN(compVW[compSId], localW);
  {
    recvQueues.push_back((resultantValue, compVC));
  }
  // Exchange shard watermarks periodically to update compVW
  // ...
}
```

**Why multiple Paxos streams?** (lines 549-553):

> "We use per-core streams rather than a single stream for the entire shard because prior works [85, 90] have shown (and we have confirmed) that the throughput of a single MultiPaxos stream plateaus after ≈ 10 worker threads due to expensive thread synchronization overhead."

#### Phase 4: Replay (On Followers)

From the paper (lines 347-352):

> "Replay: The followers need to replay from the per-core log to reconstruct the same state as the leader. Because the replication phase skips coordination across shards and cores, the dependency information across shards and cores is missing. Without such information, the replay could lead to inconsistent states."

**The Problem** (Section 4.4, Figure 3, lines 564-576):

Consider a banking example:
- **T1**: Alice transfers $100 from savings (Shard 0) to checking (Shard 1)
- **T2**: Alice transfers $100 from checking (Shard 1) to Bob (Shard 1)

If Shard 0 fails before T1 is fully replicated:
- **Issue 1**: T1's write on Shard 1 is replicated, but not on Shard 0 (incomplete)
- **Issue 2**: T2 depends on T1, but T1's Shard 0 operation is lost

**Solution: Vector Watermark** (lines 740-750):

> "To correct this, Mako introduces a lightweight, decentralized vector watermark scheme to enable safe replay. A vector watermark is an array of shard watermarks (w₀, w₁, ..., wₙ₋₁) where n equals the number of shards. Each shard watermark wᵢ is calculated by shard-i independently."

**Progress Checking** (lines 760-768):

> "In this phase, Mako actively verifies whether a transaction's version falls below the latest vector watermark. If it does, the transaction proceeds to the replay phase, indicating that all transactions it depends on, from other cores or shards, have also been replicated to a majority of replicas."

**Implementation** (Appendix D, lines 2996-3012):

```cpp
// Replay on shard followers
while (!recvQueues.empty()) {
  bool safeToReplay = true;
  for (int i = 0; i < compSize; i++) {
    if (recvQueues.front().compVC[i] > compVW[i]) {
      safeToReplay = false;
      break;
    }
  }

  if (!safeToReplay) break;

  // It is safe to return back the client and replay
  replay(recvQueues.front());
  recvQueues.pop_front();
}
```

### Key Architectural Components

#### 1. Shards

From the paper (lines 311-312):

> "A Mako deployment splits its data (key-value pairs) into multiple shards."

**Multi-Shard Single-Process Mode** (`src/mako/mako.hh:186-229`):

```cpp
// Check if running in multi-shard mode
if (benchConfig.getConfig() && benchConfig.getConfig()->multi_shard_mode) {
  // Multi-shard mode: initialize database for each local shard
  Notice("Initializing multi-shard mode with %zu local shards",
         benchConfig.getConfig()->local_shard_indices.size());

  for (int shard_idx : benchConfig.getConfig()->local_shard_indices) {
    ShardContext ctx;
    ctx.shard_index = shard_idx;
    ctx.cluster_role = benchConfig.getCluster();

    // Initialize database for this shard
    bool is_leader = benchConfig.getLeaderConfig();
    ctx.db = initShardDB(shard_idx, is_leader, ctx.cluster_role);

    // Store shard context
    benchConfig.addShardContext(shard_idx, ctx);

    Notice("Initialized ShardContext for shard %d", shard_idx);
  }
```

#### 2. Leader-Follower Architecture

From the paper (lines 312-314):

> "Each shard is replicated to multiple datacenters with a leader-follower architecture."

**Configuration** (from lines 1272-1276):

> "To ensure fault-tolerance, we also replicate each server so we have one leader, one learner (§6.3), and two followers (this means in total we have up to 40 servers)."

**Learners for Fast Recovery** (lines 1169-1177):

> "Learners. To speed up failure recovery, Mako co-locates a shard learner in the same datacenter as the shard leader. A shard learner is essentially the same as a shard follower, except that it does not vote on consensus. This allows Mako to recover from the failure of a leader more quickly"

#### 3. Worker Threads (Per-Core Streams)

**Thread Configuration** (`src/mako/mako.hh:61-62`):

```cpp
cerr << "  num-threads : " << benchConfig.getNthreads()  << endl;
```

From the evaluation (lines 1244-1246):

> "Mako, OCC+OR, 2PC: 1 server is 1 multi-core shard with 24 worker threads."

**RocksDB Persistence Thread Pool** (`src/mako/rocksdb_persistence.cc:95-99`):

```cpp
// Use the requested number of worker threads 
// Each worker handles a subset of partitions in round-robin fashion
for (size_t i = 0; i < num_threads; ++i) {
  worker_threads_.emplace_back(&RocksDBPersistence::workerThread, this, i, num_threads);
}
```

#### 4. Storage Engine

From the paper (lines 1207-1210):

> "We implemented Mako in C++. Mako is based on a few existing works, including Silo [95] for each server's database engine"

**Masstree**: Lock-free in-memory B+tree index (referenced in paper Section 1, line 95)

**Multi-Version Storage** (Appendix C, lines 2738-2751):

> "As described in the paper, Mako stores multiple versions of each key to support rollbacks in shard failures. This is implemented as a linked list. Each version contains a pointer to the previous version on this key. The key index stores the pointer to the latest version."

#### 5. Network Layer

From the paper (lines 1210-1211):

> "eRPC [45] for accelerated networks"

**Transport Backends** (from `CLAUDE.md`):

> Mako supports two RPC backends (switchable at runtime):
> - **rrr/rpc** (default): Portable TCP/IP-based RPC (~10-50 μs latency)
> - **eRPC**: High-performance RDMA-based RPC (~1-2 μs latency)

**Multi-Transport Manager** (`src/mako/mako.hh:116-169`):

```cpp
// Initialize and start transports for multi-shard mode
static bool initMultiShardTransports(const std::vector<int>& local_shard_indices) {
  auto& benchConfig = BenchmarkConfig::getInstance();
  transport::Configuration* config = benchConfig.getConfig();

  Notice("Initializing MultiTransportManager for %zu shards", local_shard_indices.size());

  // Create MultiTransportManager
  g_multi_transport_manager = new mako::MultiTransportManager();

  // Determine local IP from first shard's configuration
  std::string local_ip = config->shard(local_shard_indices[0], benchConfig.getClusterRole()).host;

  // Initialize all transports
  bool success = g_multi_transport_manager->InitializeAll(
    config->configFile,
    local_shard_indices,
    local_ip,
    benchConfig.getCluster(),
    1,  // st_nr_req_types
    12, // end_nr_req_types
    0,  // phy_port (0 for TCP)
    0   // numa_node
  );
```

### Failure Recovery

From the paper (Section 5, lines 872-880):

> "Mako groups transactions into epochs. When failures happen, Mako advances the epoch and makes a collective decision about which transactions in the previous epoch must roll back. Mako has a configuration manager (CM) that manages the epochs (which is also sometimes called a view manager in other works). CM itself is replicated so it is considered always alive."

**Finalized Vector Watermark (FVW)** (lines 918-923):

> "After a shard computes its finalized watermark, it broadcasts the watermark to all other shards. Eventually, all shards exchange their finalized watermarks to form the finalized vector watermark (FVW), which represents a consistent and maximum global cut across all shards for the old epoch."

**Rollback Decision** (lines 929-930):

> "After the FVW for the old epoch is established, any transactions that are not below the FVW are rolled back on shard leaders and forever abandoned by the system."

**Bounded Cascading Aborts** (lines 934-936):

> "Note that the number of transactions above the FVW in the old epoch does not grow once the FVW is established, which means the rollbacks are bounded; there are no unbounded and expensive cascading aborts into new epochs."

### Summary: Why This Architecture Works

From the paper's conclusion (Section 10, lines 1977-1982):

> "This paper presents Mako, a highly-available, fast, and scalable transactional database system, specifically optimized for geo-replication. Mako decouples transaction execution and replication, makes execution speculative, and leverages multi-core machines. Our experimental evaluation shows that Mako outperforms state-of-the-art geo-replicated transactional systems by an order of magnitude in throughput."

**The Three Pillars:**

1. **Multicore per shard**: Achieves high single-shard throughput (inspired by Silo)
2. **Speculative 2PC**: Hides geo-replication latency by pipelining
3. **Vector clocks + Vector watermarks**: Tracks dependencies and replication progress without centralized coordination

**Performance Gains** (from Section 7.2):

- **8.6× faster than Calvin** (deterministic database)
- **32.2× faster than OCC+OR** (optimistic replication)
- **68.4% of Silo's throughput** (acceptable overhead for geo-replication + sharding)

---

## References

- **Paper**: "Mako: Speculative Distributed Transactions with Geo-Replication" (OSDI'25)
- **Code**: Available at https://github.com/stonysystems/mako
- **Documentation**: See `doc/` directory for additional details

---

## Q4: Single-Server, 8-Core, No-Sharding Setup: Thread Roles and Transaction Lifecycle

### Scenario Setup

You have:
- **1 leader server** (no distributed sharding)
- **8 CPU cores** on the leader
- **1 thread per core** (8 worker threads total)
- **No cross-shard transactions** (all data is local to this shard)
- **Replication enabled** (with followers/learners on **separate physical servers**)

In this configuration, Mako simplifies to a **single-shard, replicated, multi-core database** similar to systems like Rolis mentioned in the paper.

### IMPORTANT CLARIFICATION: What "Replication" Means

**Replication is NOT between the 8 worker threads!** This is a critical point:

- **All 8 worker threads share the SAME Masstree** (single shared in-memory data structure)
- **Replication happens to SEPARATE physical servers** (followers in different datacenters)
- The 8 Paxos replication threads replicate to **remote followers**, not to each other

**Visual Clarification:**

```
┌─────────────────────────────────────────────────────────┐
│         Leader Server (Datacenter 1)                    │
│  ┌────────────────────────────────────────────────┐    │
│  │          Shared Masstree (in-memory)           │    │
│  │   All 8 workers read/write THIS SAME database  │    │
│  └────────────────────────────────────────────────┘    │
│         ↑  ↑  ↑  ↑  ↑  ↑  ↑  ↑                         │
│         │  │  │  │  │  │  │  │                         │
│    Worker threads (all access same Masstree):          │
│    W0  W1  W2  W3  W4  W5  W6  W7                      │
│                                                         │
│    Paxos Replication Threads:                          │
│    P0  P1  P2  P3  P4  P5  P6  P7                      │
│     │   │   │   │   │   │   │   │                      │
└─────┼───┼───┼───┼───┼───┼───┼───┼──────────────────────┘
      │   │   │   │   │   │   │   │
      │   │   │   │   │   │   │   │  (Network RPCs)
      ▼   ▼   ▼   ▼   ▼   ▼   ▼   ▼
┌─────────────────────────────────────────────────────────┐
│      Follower Server 1 (Datacenter 2)                   │
│  ┌────────────────────────────────────────────────┐    │
│  │       Replica Masstree (separate copy)         │    │
│  └────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────┐
│      Follower Server 2 (Datacenter 3)                   │
│  ┌────────────────────────────────────────────────┐    │
│  │       Replica Masstree (separate copy)         │    │
│  └────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────┘
```

**Configuration Example** (`config/local/2follower_1.yml:1-5`):

```yaml
site:
  server: # each line is a partition
    - ["s101:8001", "s201:8101", "s301:8201"]
    #    ^leader     ^follower1   ^follower2
```

This means:
- **s101** is the leader (your 8-core server)
- **s201** is follower 1 (different physical server)
- **s301** is follower 2 (different physical server)

In this configuration, Mako simplifies to a **single-shard, replicated, multi-core database** similar to systems like Rolis mentioned in the paper.

### Thread Architecture

In this setup, Mako runs **multiple categories of threads**:

#### 1. Main Thread (1 thread)
- **Role**: Initialization and coordination
- **Code**: `src/mako/benchmarks/dbtest.cc:182-233`

```cpp
// Single-shard mode: keep existing behavior
abstract_db * db = initWithDB(); // Some init is required for followers/learners
// Run worker threads on the leader
if (benchConfig.getLeaderConfig()) {
  run_workers(db);
}
```

**Responsibilities:**
- Parse configuration and command-line arguments
- Initialize database (`mbta_wrapper`)
- Initialize transport layer (eRPC or rrr/rpc)
- Spawn worker threads
- Wait for completion and cleanup

#### 2. Worker Threads (8 threads - one per core)
- **Role**: Execute transactions from clients
- **Code**: `src/mako/benchmarks/bench.cc:122-806` and `src/mako/benchmarks/tpcc.cc:3599-3633`

```cpp
virtual vector<bench_worker *> make_workers()
{
  const unsigned alignment = coreid::num_cpus_online();
  const int blockstart =
    coreid::allocate_contiguous_aligned_block(BenchmarkConfig::getInstance().getNthreads(), alignment);
  ALWAYS_ERROR(blockstart >= 0);
  ALWAYS_ERROR((blockstart % alignment) == 0);
  fast_random r(23984543);
  vector<bench_worker *> ret;
  auto& cfg = BenchmarkConfig::getInstance();
  if (NumWarehouses() <= cfg.getNthreads()) {
    for (size_t i = 0; i < cfg.getNthreads(); i++) {
      ret.push_back(
        new tpcc_worker(
          blockstart + i,  // Core ID for CPU affinity
          r.next(), db, open_tables, partitions, remote_partitions, 
          &barrier_a, &barrier_b,
          (i % NumWarehouses()) + 1, (i % NumWarehouses()) + 2));
    }
  }
  // ...
  return ret;
}
```

**CPU Affinity** (`src/mako/thread.cc:21-31`):

```cpp
void ndb_thread::startBind(int core_id)
{
  thd_ = std::move(thread(&ndb_thread::run, this));
  pthread_setname_np(thd_.native_handle(), ("worker_"+std::to_string(core_id)).c_str());
  // CPU affinity code (commented out but available):
  // cpu_set_t cpuset;
  // CPU_ZERO(&cpuset);
  // CPU_SET(core_id, &cpuset);
  // pthread_setaffinity_np(thd_.native_handle(), sizeof(cpu_set_t), &cpuset);
  if (daemon_)
    thd_.detach();
}
```

**Responsibilities:**
- Each worker thread is pinned to a specific CPU core
- Continuously execute transactions (TPC-C, microbenchmark, etc.)
- Perform optimistic reads from Masstree
- Execute OCC validation and commit protocol
- Buffer writes during transaction execution
- **No cross-shard coordination needed** (single shard)

#### 3. Transport Event Loop Thread (1 thread)
- **Role**: Handle network I/O for replication and client requests
- **Code**: `src/mako/lib/fasttransport.cc:213-217` and `src/mako/lib/rrr_rpc_backend.cc:515-567`

**For rrr/rpc backend:**

```cpp
void FastTransport::Run()
{
  Assert(backend_ != nullptr);
  backend_->RunEventLoop();
}

void RrrRpcBackend::RunEventLoop() {
  // The PollThread runs its own thread for network I/O
  // Here we process responses from helper threads and send them back
  Notice("RrrRpcBackend::RunEventLoop: Starting event loop");

  event_loop_running_.store(true, std::memory_order_release);

  while (!stop_) {
    // Process responses from all helper queues
    for (auto& it : queue_holders_response_) {
      // Fetch responses from helper thread queue
      while (!server_queue->is_req_buffer_empty()) {
        server_queue->fetch_one_req(&req_handle_ptr, msg_size);
        // Send response back via rrr/rpc
        // ...
      }
    }
  }
}
```

**For eRPC backend:**
- Uses eRPC's event loop for ultra-low latency (~1-2 μs)
- Runs `erpc::Rpc::run_event_loop()` continuously

**Responsibilities:**
- Accept incoming RPC requests (from clients, followers, learners)
- Poll network sockets (using epoll/libevent)
- Dispatch requests to worker threads
- Send responses back after processing
- Handle Paxos replication messages

#### 4. Paxos Replication Threads (8 threads - one per worker)
- **Role**: Replicate transaction logs to **remote follower servers**
- **Code**: From paper Section 4.3, lines 544-560

From the paper:
> "Each shard runs multiple Paxos instances, one for each core (worker thread), to replicate transactions' logs to followers after certification."

**CRITICAL: These threads replicate to REMOTE servers, not to each other!**

**Per-Core Paxos Stream Architecture** (`src/mako/rocksdb_persistence.cc:23-103`):

```cpp
bool RocksDBPersistence::initialize(const std::string& db_path, size_t num_partitions, size_t num_threads,
                                    uint32_t shard_id, uint32_t num_shards) {
  num_partitions_ = num_partitions; // the number of worker thread per shard

  // Initialize per-partition queues
  partition_queues_.resize(num_partitions_);
  for (size_t i = 0; i < num_partitions_; ++i) {
    partition_queues_[i] = std::make_unique<PartitionQueue>();
  }

  // Create separate database file for each partition
  // NOTE: This is for PERSISTENCE (disk), not for worker thread separation!
  partition_dbs_.resize(num_partitions_);
  for (size_t partition_id = 0; partition_id < num_partitions_; ++partition_id) {
    std::string partition_db_path = db_path + "_partition" + std::to_string(partition_id);
    rocksdb::DB* db_raw;
    rocksdb::Status status = rocksdb::DB::Open(options_, partition_db_path, &db_raw);
    // ...
    partition_dbs_[partition_id].reset(db_raw);
  }
}
```

**Why per-core Paxos streams?** From paper (lines 549-553):
> "We use per-core streams rather than a single stream for the entire shard because prior works [85, 90] have shown (and we have confirmed) that the throughput of a single MultiPaxos stream plateaus after ≈ 10 worker threads due to expensive thread synchronization overhead."

**What this means:**
- 8 independent Paxos protocols running in parallel
- Each sends RPCs to the **same remote follower servers**
- Avoids bottleneck of a single replication queue
- Follower receives 8 independent streams and must reassemble them safely

**Responsibilities:**
- Each replication thread corresponds to one worker thread
- Batches committed transactions from its worker (batch size = 400)
- Runs MultiPaxos protocol to replicate to **remote follower servers** over the network
- Maintains independent replication progress (no cross-thread coordination)
- Updates vector watermark after successful replication

#### 5. RRR/RPC Poll Thread (1 thread)
- **Role**: Network I/O polling using epoll
- **Code**: `src/rrr/reactor/reactor.cc:255-289`

```cpp
void PollThreadWorker::poll_loop() {
  Log_debug("[poll_loop] Starting poll loop");
  while (!stop_) {
    TriggerJob();

    // Wait for events (epoll_wait with short timeout)
    // Pass callback to handle mode updates from handle_write() return values
    poll_.Wait([this](Pollable* poll, int new_mode) {
      do_update_mode(poll->fd(), new_mode, poll);
    });

    // Process commands from channel (non-blocking try_recv)
    process_commands();

    TriggerJob();

    // Process deferred removals
    process_pending_removals();

    TriggerJob();
    Reactor::GetReactor()->Loop();
  }
}
```

**Responsibilities:**
- Continuously poll network file descriptors using `epoll_wait()`
- Detect incoming network events (readable/writable sockets)
- Dispatch to appropriate handlers
- Handle connection management

### Transaction Lifecycle in 8-Core Single-Shard Setup

Let's trace a complete transaction from start to finish:

#### Phase 0: Client Request Arrives

```
[Network] → [Transport Event Loop Thread] → [Dispatch to Worker Thread]
```

1. Client sends transaction request over network
2. **Transport Event Loop Thread** receives it via epoll
3. Request is queued for an available **Worker Thread**

#### Phase 1: Transaction Execution (Worker Thread)

**Code**: Appendix D from paper, lines 2907-2920

```cpp
// Worker Thread executes transaction
// Execution phase
{
  // Read() optimistically, and buffer writes
  tuple<int, vector<int>, ValueType> Read(key, currentEpoch) {
    _epoch, _compVC, _value = masstree[key][0];  // Read from Masstree
    return {_epoch, _compVC, _value};
  }
  
  // For writes, buffer in WriteSet (not installed yet)
  WriteSet.push({key, new_value});
}
```

**What happens:**
- Worker thread reads from **Masstree** (lock-free B+tree)
- Reads are **optimistic** - no locks acquired
- Reads return latest committed version (including speculative)
- Writes are buffered in **WriteSet**, not yet visible
- ReadSet tracks all read versions for validation

**Timeline:**
- **Duration**: ~1-10 microseconds (in-memory operations)
- **No network I/O** (single shard, no cross-shard reads)

#### Phase 2: Transaction Certification (Worker Thread)

**Code**: Appendix D from paper, lines 2939-2973

Since this is a **single-shard setup**, the 2PC protocol simplifies significantly - **no network RPCs needed**!

**Normal multi-shard 2PC** (4 rounds of RPCs):
1. Lock (RPC to all shards)
2. GetClock (RPC to all shards)
3. Validate (RPC to all shards)
4. Install (RPC to all shards)

**Single-shard 2PC** (all local):

```cpp
// Commit phase - all local, no RPCs!
{
  // Phase 1: Lock keys in writeset (local)
  for (auto key : WriteSet) {
    if (!acquire_lock(key)) {
      abort();  // Lock conflict
    }
  }

  // Phase 2: Get clock (local)
  vector<int> compVC = getClock();  // Just increment local counter
  // For single shard: compVC = (local_counter++, 0, 0, ..., 0)

  // Phase 3: Validate keys in readset (local OCC)
  for (auto [key, version] : ReadSet) {
    if (masstree[key].version != version || masstree[key].locked) {
      abort();  // Read-write conflict detected
    }
  }

  // Phase 4: Install & Release locks (local)
  Install(writeSet, currentEpoch, compVC);
}

void Install(writeSet, currentEpoch, compVC) {
  for (auto [k, v] : writeSet) {
    masstree[k].insert(0, (currentEpoch, compVC, v));  // Speculatively install
    int delta = compVC[0] - localCounter;  // Update local clock
    if (delta > 0)
      localCounter.fetch_and_add(delta);
    unlock(k);  // Release lock
  }
}
```

**What happens:**
- All 4 phases execute **locally** within the worker thread
- No network latency - pure in-memory operations
- Uses OCC (Optimistic Concurrency Control) like Silo
- Writes become **speculatively visible** to other transactions
- Transaction moves to **CERTIFIED** state

**Timeline:**
- **Duration**: ~1-5 microseconds (lock acquisition + validation)
- **State**: Transaction is certified but **not durable yet**

#### Phase 3: Asynchronous Replication (Paxos Replication Thread)

**Code**: Appendix D from paper, lines 2975-2994

```cpp
// Worker thread adds transaction log to batch
add_to_replication_queue(transaction_log);

// Paxos Replication Thread (runs in background)
void callbackAsyncPaxosRep(int threadID, vector<int> compVC, 
                           int locShardID, const string& resultantValue) {
  compSId = getCompSIdBySId(locShardID);
  replicateProgress[threadID] = compVC[compSId];  // Update progress
  localW = MIN(replicateProgress);  // Compute local watermark
  compVW[compSId] = MIN(compVW[compSId], localW);  // Update vector watermark
  
  // Queue for followers to replay
  recvQueues.push_back((resultantValue, compVC));
  
  // Periodically gossip vector watermark to other shards
  // (In single-shard setup, this is trivial)
}
```

**What happens:**
1. Worker thread continues executing **next transaction** immediately
2. **Paxos Replication Thread** (corresponding to this worker) picks up the log
3. Batches transactions (400 per batch) to amortize RPC overhead
4. Runs MultiPaxos protocol:
   - **PROPOSE** → Send to followers
   - Wait for **PROMISE** from majority
   - **ACCEPT** → Commit to followers
   - Wait for **ACCEPTED** from majority
5. Once majority acknowledges, transaction becomes **durable**
6. Update **local watermark** to track replication progress

**Timeline:**
- **Duration**: 50-100 milliseconds (WAN RTT to followers)
- **Parallelism**: Worker thread is **not blocked**!
- **State**: Transaction moves to **COMMITTED** once replicated

#### Phase 4: Replay on Followers (Follower Worker Threads)

**Code**: `src/mako/mako.hh:210-366` and Appendix D, lines 2996-3012

```cpp
// On Follower: register callback for each worker thread
register_paxos_follower_callback(replicated_db, thread_id);

// Follower callback (invoked by Paxos when log arrives)
register_for_follower_par_id_return([&,thread_id](
    const char*& log, int len, int par_id, int slot_id, 
    std::queue<std::tuple<int, int, int, int, const char *>> & un_replay_logs_) {
  
  // Parse transaction log
  CommitInfo commit_info = get_latest_commit_info((char *) log, len);
  timestamp = commit_info.timestamp;
  
  // Update local timestamp for this worker thread
  sync_util::sync_logger::local_timestamp_[par_id].store(commit_info.timestamp, memory_order_release);
  
  // Check if safe to replay (progress checking)
  while (!recvQueues.empty()) {
    bool safeToReplay = true;
    for (int i = 0; i < compSize; i++) {
      if (recvQueues.front().compVC[i] > compVW[i]) {
        safeToReplay = false;  // Dependencies not yet replicated
        break;
      }
    }

    if (!safeToReplay) break;

    // Safe to replay: all dependencies replicated
    replay(recvQueues.front());
    recvQueues.pop_front();
  }
}, thread_id);
```

**What happens:**
1. Follower receives replicated log from leader
2. **Progress checking**: Compare transaction's vector clock with vector watermark
3. If `compVC[i] <= compVW[i]` for all i, **safe to replay**
4. Apply transaction's writes to follower's Masstree
5. If dependencies missing, **queue for later** replay

**Timeline:**
- **Duration**: ~1-10 microseconds per transaction (in-memory replay)
- **Condition**: Only replay when all dependencies are replicated

#### Phase 5: Client Response (Transport Thread)

```
[Worker Thread] → [Replication Complete] → [Transport Thread] → [Network] → [Client]
```

**What happens:**
1. After majority replication completes, transaction moves to **COMMITTED**
2. Worker thread (or transport thread) sends response to client
3. Client receives confirmation

**From client's perspective:**
- **Total latency**: 50-150 milliseconds
  - Execution: ~1-10 μs
  - Certification: ~1-5 μs
  - Replication: 50-100 ms (dominant factor!)
  - Network: ~1-10 ms

### Answering Your Question: What Gets Replicated Where?

**Question**: "Do all 8 cores share the same masstree and replication is between the 8 cores? So if worker thread 1 processes txn 1, will it be replicated in worker thread 2?"

**Answer**: **NO!** This is a crucial misconception to clear up:

#### What Actually Happens:

1. **All 8 worker threads share ONE Masstree**
   - There is a **single shared in-memory Masstree** on the leader server
   - Worker 1, Worker 2, ..., Worker 8 all read and write to the **same data structure**
   - When Worker 1 commits a transaction, it's **immediately visible** to Worker 2, 3, ..., 8
   - No replication needed between workers - they're all accessing the same memory!

2. **Replication goes to REMOTE servers**
   - The 8 Paxos replication threads send transaction logs over the **network**
   - Destination: **Follower servers in different datacenters** (e.g., s201, s301)
   - These are completely separate physical machines, possibly thousands of miles away

3. **Why 8 separate Paxos streams?**
   - **Not** to replicate between workers on the same machine
   - **Instead**: To parallelize the replication to remote followers
   - Each stream independently sends to the same remote servers
   - Avoids bottleneck of serializing all transactions into one queue

#### Example Flow:

```
Leader Server (Datacenter 1 - New York):
┌───────────────────────────────────────┐
│  Shared Masstree (one copy)          │
│    Key "Alice" → Balance: $100       │
│    Key "Bob" → Balance: $50          │
│                                       │
│  Worker 1: Updates Alice → $150      │
│  Worker 2: Reads Bob (sees $50)      │  ← Both access SAME Masstree
│  Worker 3: Updates Bob → $60         │
│                                       │
│  Paxos Stream 0 (for Worker 0):      │
│    └─→ Sends logs over network ──────┼──────┐
│  Paxos Stream 1 (for Worker 1):      │      │
│    └─→ Sends logs over network ──────┼──────┤
│  ...                                  │      │
│  Paxos Stream 7 (for Worker 7):      │      │
│    └─→ Sends logs over network ──────┼──────┤
└───────────────────────────────────────┘      │
                                               │ (WAN Network)
                                               │ 50ms RTT
                                               │
                     ┌─────────────────────────┼─────────────────────┐
                     ▼                         ▼                     ▼
Follower 1 (Datacenter 2 - California):    Follower 2 (Datacenter 3 - Europe):
┌───────────────────────────────────────┐ ┌───────────────────────────────────────┐
│  Replica Masstree (separate copy)    │ │  Replica Masstree (separate copy)    │
│    Key "Alice" → Balance: $100       │ │    Key "Alice" → Balance: $100       │
│    Key "Bob" → Balance: $50          │ │    Key "Bob" → Balance: $50          │
│                                       │ │                                       │
│  Receives 8 Paxos streams             │ │  Receives 8 Paxos streams             │
│  Replays transactions to match leader │ │  Replays transactions to match leader │
└───────────────────────────────────────┘ └───────────────────────────────────────┘
```

#### Why This Design?

**Purpose of multiple worker threads:**
- **Parallelism**: Utilize all 8 CPU cores for transaction execution
- **Throughput**: Process 8 transactions simultaneously
- **Lock-free**: Masstree allows concurrent reads without blocking

**Purpose of multiple Paxos streams:**
- **Avoid serialization bottleneck**: Don't force all 8 workers to share one replication queue
- **Independent progress**: Each worker's transactions can replicate at different rates
- **Scalability**: From paper: "single MultiPaxos stream plateaus after ≈10 worker threads"

**Purpose of replication to remote servers:**
- **Fault tolerance**: If leader server crashes, followers have a copy
- **Geo-distribution**: Survive datacenter failures
- **Disaster recovery**: Data is safe even if entire datacenter goes down

#### What Would Happen Without Remote Replication?

If the leader server (with all 8 workers) crashes:
- **Without replication**: All data is **LOST** (it was only in memory)
- **With replication**: Followers promote a new leader, data is **SAFE**

### Concurrency: How 8 Worker Threads Work Together

#### Shared Data Structures

1. **Masstree (Lock-free B+tree)**
   - All 8 worker threads read/write concurrently
   - Uses optimistic concurrency control
   - Lock-free reads, atomic updates for writes

2. **Per-Worker Replication Queues**
   - Each worker has independent queue
   - No contention between workers

3. **Vector Watermark** (shared, rarely updated)
   - Updated only after replication completes
   - Lock-free atomic updates

#### Example: 3 Transactions in Parallel

```
Timeline:
T0 (Worker 0):  [Execute] [Certify] [Install] → [Replicate in background]
T1 (Worker 1):     [Execute] [Certify] [Install] → [Replicate]
T2 (Worker 2):        [Execute] [Certify] [Install] → [Replicate]

If T1 reads key written by T0:
- T0 certifies at time 100, installs speculatively
- T1 executes at time 105, reads T0's speculative write (allowed!)
- T1 gets vector clock > T0's vector clock
- If T0 later fails replication, T1 must also rollback (cascading abort)
```

#### Throughput Calculation

From the paper (Section 7.9, lines 1882-1898):

**Single-shard Mako** with 8 workers:
- Base throughput: **1.66M TPS** (from Silo)
- With multi-version + replication overhead: **~1.14M TPS** (68.4% of Silo)
- **Per-worker**: ~142K TPS per worker thread

### Summary: Thread Roles

| Thread Type | Count | Role | Blocks? | CPU Usage |
|-------------|-------|------|---------|-----------|
| **Main Thread** | 1 | Initialization, cleanup | Yes (waits for workers) | Low |
| **Worker Threads** | 8 | Execute transactions | No (non-blocking) | **100%** |
| **Transport Event Loop** | 1 | Network I/O | Blocks on epoll | Low-Med |
| **Paxos Replication Threads** | 8 | Replicate logs | Blocks on network | Low-Med |
| **RRR/RPC Poll Thread** | 1 | epoll network polling | Blocks on epoll | Low |

**Total threads**: ~19 threads

### Key Insights for Single-Shard Setup

1. **All workers share ONE database**: The 8 worker threads all access the same in-memory Masstree. There's no replication between worker threads!

2. **No distributed coordination overhead**: Since there's only one shard, the 4-round 2PC protocol becomes local-only operations (no network RPCs between workers).

3. **Replication is to REMOTE servers**: The 8 Paxos threads send logs to followers in different datacenters, NOT to each other.

4. **Speculation is still crucial**: Even in single-shard mode, speculation allows worker threads to execute the next transaction while replication to remote followers happens in the background.

5. **Per-core Paxos streams**: Each of the 8 workers has its own independent Paxos stream to remote followers, avoiding synchronization bottlenecks on the leader.

6. **Vector clocks simplify**: In single-shard mode, vector clock becomes almost a scalar:
   - `(local_counter, 0, 0, ..., 0)` for all transactions
   - But the infrastructure remains for future sharding

7. **Bottleneck**: Replication latency to remote followers (50-100ms WAN RTT), not CPU or concurrency control on the leader.

8. **Throughput**: Limited by replication bandwidth to followers, not worker thread count. Adding more workers beyond ~24 doesn't help much because the 8-24 Paxos streams start to saturate the replication bandwidth.

### Summary: What Actually Gets Replicated

**WRONG Understanding** ❌:
```
Worker 1 → Replicates to → Worker 2, 3, 4, 5, 6, 7, 8 (on same machine)
```

**CORRECT Understanding** ✅:
```
All 8 Workers → Share Same Masstree (no replication needed)
              ↓
8 Paxos Threads → Replicate to → Follower Servers in other datacenters
```

**The Point:**
1. **Workers share data locally** → Fast (nanoseconds), no network
2. **Paxos replicates remotely** → Slow (50-100ms), over WAN
3. **Speculation hides WAN latency** → Workers don't wait for remote replication
4. **Multiple Paxos streams** → Parallelize the remote replication bottleneck

### Comparison: With vs. Without Sharding

| Aspect | Single-Shard (8 cores) | Multi-Shard (8 cores each) |
|--------|------------------------|---------------------------|
| Worker threads | 8 total (share 1 Masstree) | 8 per shard (separate Masstrees) |
| 2PC RPCs | 0 (all local) | 4 rounds × #shards (network) |
| Vector clock | (N, 0, ..., 0) | (N₀, N₁, ..., Nₖ) |
| Throughput | ~1.14M TPS | ~0.36M TPS per shard |
| Replication | 8 Paxos streams to remote followers | 8 × #shards Paxos streams |
| Remote replicas | 2-4 follower servers | 2-4 follower servers per shard |


## Q5: Entry Points, Deployment Options, and Code Architecture

### Main Entry Points

Mako has **multiple entry points** depending on your use case:

#### 1. **dbtest** (Primary Entry Point for Benchmarking)
- **File**: `src/mako/benchmarks/dbtest.cc:144`
- **Purpose**: Run benchmarks (TPC-C, YCSB, microbenchmarks)
- **Usage**: `./build/dbtest --bench tpcc --num-threads 8 --runtime 30`

#### 2. **s_main** (Deptran/Janus Framework Entry Point)
- **File**: `src/deptran/s_main.cc:134`
- **Purpose**: Run Deptran protocols (Janus, 2PL, OCC, Paxos, etc.)
- **Usage**: `./build/s_main -f config/janus_config.yml`

#### 3. **Python Scripts** (Orchestration)
- **File**: `run.py`
- **Purpose**: Orchestrate multi-server deployments

#### 4. **Standalone Examples**
- **Files**: `examples/*.cc`
- **Examples**: `simpleTransaction.cc`, `simplePaxos.cc`, `continuousTransactions.cc`

### Can You Deploy Mako Standalone?

**Answer**: Yes, but with caveats. Mako is currently **tightly coupled** with the benchmark framework.

**Recommended**: Use `dbtest` as your server base and customize transaction logic.

**Challenges for pure standalone**:
- Benchmark code intertwined with database code
- No clean "library" interface yet
- Configuration system assumes benchmark parameters

### Understanding `mbta_wrapper` Architecture

`mbta_wrapper` is the **glue layer** connecting benchmarks to storage:

#### Complete Architecture Stack (6 Layers)

```
Layer 1: Benchmark Interface (abstract_db)
  ↓  Pure virtual interface for transactions
Layer 2: Adapter (mbta_wrapper)
  ↓  Adapts STO transaction API
Layer 3: Index Wrapper (mbta_ordered_index)
  ↓  Wraps MassTrans, handles local/remote access
Layer 4: Sharding Router (mbta_sharded_ordered_index)
  ↓  Routes to local/remote partitions by hash
Layer 5: Transaction Engine (MassTrans)
  ↓  OCC protocol, read/write sets
Layer 6: Storage (Masstree)
     Lock-free concurrent B+tree
```

#### Key Classes

| Class | Purpose | File |
|-------|---------|------|
| `abstract_db` | Benchmark DB interface | `abstract_db.h` |
| `mbta_wrapper` | Adapts STO to abstract_db | `mbta_wrapper.hh` |
| `mbta_ordered_index` | Single table wrapper | `mbta_wrapper.hh` |
| `mbta_sharded_ordered_index` | Multi-shard router | `mbta_sharded_ordered_index.hh` |
| `MassTrans<T>` | Transactional Masstree | `MassTrans.hh` |
| `Masstree` | Lock-free B+tree | `masstree/` |

### Entry Points Summary

| Entry Point | Purpose | Standalone? |
|-------------|---------|-------------|
| **dbtest** | Benchmark runner | ✅ Easiest to adapt |
| **s_main** | Deptran protocols | ⚠️ Requires framework |
| **Examples** | Simple tests | ✅ Yes, minimal deps |
| **Python** | Orchestration | ⚠️ For distributed only |

. What is the “transaction layer” in Mako?
At a high level there are two layers of transaction logic:
Intra‑shard transaction engine (what you’re asking about):
Implements Silo‑style OCC on top of Masstree.
Lives mostly in the sto code (Transaction, MassTrans, TThread, etc.) and the older txn.* path.
Distributed/speculative layer:
Assigns vector/timestamps, runs speculative 2PC and Paxos, and uses watermarks for safe replay (the OSDI paper’s contribution).
Lives in txn_proto2_impl.*, sync_util.*, and the Paxos callbacks in mako.hh.
Everything the benchmarks see goes through abstract_db → mbta_wrapper → STO/MassTrans → Masstree.
2. What is STO?
STO is the Software Transactional Objects library used as Mako’s single‑node transaction engine:
Code location: src/mako/benchmarks/sto/
Transaction.hh / Transaction.cc: main transactional object / OCC logic
Interface.hh: TThread thread‑local state, per‑thread Transaction pointer, shard index, etc.
MassTrans.hh: transactional wrapper around Masstree tables
Core ideas (matches Silo‑style OCC in the paper §4.2):
Each transaction tracks a read set and write set over Masstree records.
Records have a version/tid (TransactionTid) that encodes lock bits + thread id and increments by increment_value on each write.
On commit, STO:
Acquires locks on write records (TransactionTid::try_lock / lock_write).
Validates that all read versions are unchanged (opacity checks in Transaction::hard_check_opacity).
Applies writes and bumps record versions.
On abort, it:
Releases locks and discards buffered writes (Transaction::stop(false, …)).
In Mako’s integration, STO is invoked via mbta_wrapper:
mbta_wrapper_arena.hh (and variants) wires abstract_db to STO:
Start a transaction:
Sto::start_transaction(); in mbta_wrapper::new_txn(...)
Commit:
Sto::commit(); in mbta_wrapper::commit_txn
Abort:
Sto::abort(); in mbta_wrapper::abort_txn
Index operations:
mbta_ordered_index::get/put/insert/remove call mbta.transGet/transPut/transInsert/... where mbta is a MassTrans instance (Masstree + OCC).
So STO = per‑node transaction engine; it corresponds to the paper’s “extends Silo’s single-node OCC protocol to a distributed variant” (paper §4.2).
3. Where are transactions batched, and how does the watermark fit in?
There are two distinct mechanisms:
3.1 Batching of committed transactions into Paxos logs
Batching is done in the STO → Paxos logging bridge, not inside STO’s OCC itself:
File: txn_proto2_impl.h (near the top) and StringAllocator class.
Each worker has a StringAllocator that:
Accumulates multiple committed transactions’ log records into a single byte buffer (LOG).
Tracks:
entries (how many txns in the batch),
curr_pos (current buffer offset),
batch_size and max_bytes_size.
When checkPushRequired() is true (enough entries or ~90% of buffer full), or in the destructor:
It writes a single latest_commit_timestamp for the batch to the end of the buffer.
It then calls add_log_to_nc(...) to hand the batch off to the Paxos layer.
Optionally it also enqueues the buffer for async RocksDB persistence.
This is the code realization of the paper’s “each Paxos stream entry corresponds to a batch of transactions” (§4.3) and “summary vector clock per batch” (§4.4; here compressed to a single timestamp).
3.2 Watermark computation and use
The watermark is computed and used in sync_util::sync_logger:
File: benchmarks/sto/sync_util.hh
local_timestamp_[i]: per‑Paxos‑stream latest replicated timestamp.
single_watermark_: global scalar watermark for the shard (timestamp10 + epoch).
computeLocal() / advancer():
Periodically take the min over all streams’ timestamps (and disk timestamps if enabled).
Advance single_watermark_ to that min.
safety_check(timestamp):
Returns true iff timestamp <= single_watermark_.
The Paxos callbacks in mako.hh update local_timestamp_[par_id] with the timestamp carried in each batch log as it becomes durable on followers, then replay logs only when safety_check against the watermark passes.
Conceptually:
Batching: “group multiple transactions into a Paxos log entry with a summary timestamp”.
Watermark: “monotone lower bound on safely replicated timestamps”, used for:
Deciding when it is safe to replay on followers (paper §4.4 progress-checking).
Deciding when a transaction is fully committed and can be acknowledged to clients (paper §4.4 & §4.5).
Transactions are not batched by the watermark; instead:
They are batched by StringAllocator based on entries / buffer size.
The watermark is updated after batches are replicated, and used as a safety gate for replay and client acks.
4. If you had to change the transaction engine, where would you do it?
There are two natural levels to change it, depending on how deep you want to go.
4.1 Swap out STO as the per-node engine (keep Mako’s distributed logic)
This is the cleanest conceptual boundary:
Primary adaptation point: mbta_wrapper and its indexes:
src/mako/benchmarks/mbta_wrapper_arena.hh (and mbta_wrapper.hh / mbta_wrapper_norm.hh):
mbta_wrapper::new_txn – currently calls Sto::start_transaction().
mbta_wrapper::commit_txn – currently calls Sto::commit().
mbta_wrapper::abort_txn / abort_txn_local – currently call Sto::abort().
mbta_ordered_index:
get/put/insert/remove call into MassTrans methods (transGet, transPut, etc.).
To plug in a new engine, you’d:
Implement a new transaction object / API (e.g., your own OCC or 2PL).
Replace the STO calls in mbta_wrapper_* with your engine’s start/commit/abort.
Replace or adapt mbta_ordered_index so that it routes get/put into your engine’s index/data structure instead of MassTrans.
As long as you preserve:
The abstract_db contract (new_txn/commit_txn/abort_txn/open_index),
And expose a per‑transaction commit timestamp that the logging layer can use,
the higher‑level Mako machinery (Paxos, watermark, failure recovery) can stay largely unchanged.
4.2 Change the lower-level OCC semantics but keep STO
If you want to keep the STO API but change its behavior (e.g., different validation/locking policy):
Work in:
src/mako/benchmarks/sto/Transaction.hh / .cc – transaction state machine, read/write sets, opacity checks.
src/mako/benchmarks/sto/MassTrans.hh – how reads/writes hit Masstree and how commit’s check() and install() work.
Optionally, the older Silo path in txn.h / txn_impl.h / txn_proto2_impl.h if you want to hook into that infrastructure instead.
Here you can, for example, implement:
Different lock acquisition order,
Per‑key locking instead of OCC,
Different read validation strategy, etc.
But you must still ensure:
A monotonic commit timestamp is available per transaction (what eventually goes into latest_commit_timestamp in StringAllocator).
Correctness relative to Masstree’s multi-version behavior.
4.3 If you change the timestamp model
If your new engine uses a different notion of commit order (e.g., Lamport clocks, HLC, something else), you also need to touch:
Where timestamps are attached to log batches:
StringAllocator::update_commit_id() and where latest_commit_timestamp is appended before add_log_to_nc.
Watermark computation:
sync_util::sync_logger’s assumptions that timestamps are scalar, comparable, and monotonically increasing.
Paxos callbacks:
Where they decode the batch timestamp from the log and update local_timestamp_[par_id].
The invariants from the paper you must keep:
If T₂ (transitively) depends on T₁, then TS(T₂) > TS(T₁).
The watermark is always a lower bound on all fully replicated commit timestamps at each shard.
Replay / client ack only when a transaction’s TS is ≤ the watermark (per shard, or vectorized if you reintroduce full vector clocks).
Short answer to “where would I change the transaction engine?”
High-level swap (recommended): replace STO by editing mbta_wrapper_* and mbta_ordered_index (and possibly mbta_sharded_ordered_index) to call your new engine instead of Sto::... / MassTrans.
Low-level behavior change: modify STO internals in benchmarks/sto/Transaction.* and MassTrans.hh.
Ensure your engine still provides:
A well‑defined commit order (timestamps),
Read/write sets (or equivalent) for correctness,
Hooks so the logging + watermark code can observe commit progress.

---

## References

- **Paper**: "Mako: Speculative Distributed Transactions with Geo-Replication" (OSDI'25)
- **Code**: Available at https://github.com/stonysystems/mako
- **Documentation**: See `doc/` directory for additional details

Perfect! Let me create a summary for you:
Summary
I've successfully added Q5: Entry Points, Deployment Options, and Code Architecture to doc/qa.md which comprehensively answers your questions:
1. Main Entry Points
Four entry points identified:
dbtest (src/mako/benchmarks/dbtest.cc) - Primary entry point
Runs benchmarks (TPC-C, YCSB, microbenchmarks)
Easiest to adapt for custom applications
Usage: ./build/dbtest --bench tpcc --num-threads 8
s_main (src/deptran/s_main.cc) - Deptran/Janus framework
For running distributed transaction protocols
Requires Deptran framework
Python scripts (run.py) - Orchestration
Multi-server deployment and experimentation
Examples (examples/*.cc) - Standalone test programs
simpleTransaction.cc, simplePaxos.cc, etc.
Minimal dependencies, good starting point
2. Standalone Deployment
Can you deploy without benchmarks? Yes, but with caveats:
Recommended: Use dbtest as base, customize transaction logic
Challenge: Code is tightly coupled with benchmark framework
No clean library interface yet (research prototype)
Would require: Extracting core components (mbta_wrapper, Masstree, transport, Paxos)
3. mbta_wrapper and Layer Architecture
Explained the 6-layer architecture:
Layer 1: abstract_db (pure virtual interface for benchmarks)Layer 2: mbta_wrapper (adapter - connects benchmarks to STO)Layer 3: mbta_ordered_index (single table wrapper)Layer 4: mbta_sharded_ordered_index (sharding router)Layer 5: MassTrans (transaction engine with OCC)Layer 6: Masstree (lock-free concurrent B+tree)
Key insight: mbta_wrapper is the glue layer that:
Adapts STO/Masstree transaction API to generic abstract_db interface
Allows benchmarks to work with different storage engines
Handles local vs. remote (sharded) access transparently
Manages thread-local storage and exception translation
Classes Breakdown
Class	Purpose
abstract_db	Generic DB interface for benchmarks
mbta_wrapper	Adapter connecting benchmarks → STO/Masstree
mbta_ordered_index	Wraps single Masstree table, handles local/RPC
mbta_sharded_ordered_index	Routes operations to correct shard by hash
MassTrans	Transactional Masstree with OCC validation
Masstree	Underlying lock-free B+tree storage
The document now provides a complete understanding of Mako's entry points, deployment options, and the detailed layered architecture!
Explain the transaction layer, what is sto, where are transactioned batched by the watermark, if I had to change the transaction engine where would it be

1. What is the “transaction layer” in Mako?
At a high level there are two layers of transaction logic:
Intra‑shard transaction engine (what you’re asking about):
Implements Silo‑style OCC on top of Masstree.
Lives mostly in the sto code (Transaction, MassTrans, TThread, etc.) and the older txn.* path.
Distributed/speculative layer:
Assigns vector/timestamps, runs speculative 2PC and Paxos, and uses watermarks for safe replay (the OSDI paper’s contribution).
Lives in txn_proto2_impl.*, sync_util.*, and the Paxos callbacks in mako.hh.
Everything the benchmarks see goes through abstract_db → mbta_wrapper → STO/MassTrans → Masstree.
2. What is STO?
STO is the Software Transactional Objects library used as Mako’s single‑node transaction engine:
Code location: src/mako/benchmarks/sto/
Transaction.hh / Transaction.cc: main transactional object / OCC logic
Interface.hh: TThread thread‑local state, per‑thread Transaction pointer, shard index, etc.
MassTrans.hh: transactional wrapper around Masstree tables
Core ideas (matches Silo‑style OCC in the paper §4.2):
Each transaction tracks a read set and write set over Masstree records.
Records have a version/tid (TransactionTid) that encodes lock bits + thread id and increments by increment_value on each write.
On commit, STO:
Acquires locks on write records (TransactionTid::try_lock / lock_write).
Validates that all read versions are unchanged (opacity checks in Transaction::hard_check_opacity).
Applies writes and bumps record versions.
On abort, it:
Releases locks and discards buffered writes (Transaction::stop(false, …)).
In Mako’s integration, STO is invoked via mbta_wrapper:
mbta_wrapper_arena.hh (and variants) wires abstract_db to STO:
Start a transaction:
Sto::start_transaction(); in mbta_wrapper::new_txn(...)
Commit:
Sto::commit(); in mbta_wrapper::commit_txn
Abort:
Sto::abort(); in mbta_wrapper::abort_txn
Index operations:
mbta_ordered_index::get/put/insert/remove call mbta.transGet/transPut/transInsert/... where mbta is a MassTrans instance (Masstree + OCC).
So STO = per‑node transaction engine; it corresponds to the paper’s “extends Silo’s single-node OCC protocol to a distributed variant” (paper §4.2).
3. Where are transactions batched, and how does the watermark fit in?
There are two distinct mechanisms:
3.1 Batching of committed transactions into Paxos logs
Batching is done in the STO → Paxos logging bridge, not inside STO’s OCC itself:
File: txn_proto2_impl.h (near the top) and StringAllocator class.
Each worker has a StringAllocator that:
Accumulates multiple committed transactions’ log records into a single byte buffer (LOG).
Tracks:
entries (how many txns in the batch),
curr_pos (current buffer offset),
batch_size and max_bytes_size.
When checkPushRequired() is true (enough entries or ~90% of buffer full), or in the destructor:
It writes a single latest_commit_timestamp for the batch to the end of the buffer.
It then calls add_log_to_nc(...) to hand the batch off to the Paxos layer.
Optionally it also enqueues the buffer for async RocksDB persistence.
This is the code realization of the paper’s “each Paxos stream entry corresponds to a batch of transactions” (§4.3) and “summary vector clock per batch” (§4.4; here compressed to a single timestamp).
3.2 Watermark computation and use
The watermark is computed and used in sync_util::sync_logger:
File: benchmarks/sto/sync_util.hh
local_timestamp_[i]: per‑Paxos‑stream latest replicated timestamp.
single_watermark_: global scalar watermark for the shard (timestamp10 + epoch).
computeLocal() / advancer():
Periodically take the min over all streams’ timestamps (and disk timestamps if enabled).
Advance single_watermark_ to that min.
safety_check(timestamp):
Returns true iff timestamp <= single_watermark_.
The Paxos callbacks in mako.hh update local_timestamp_[par_id] with the timestamp carried in each batch log as it becomes durable on followers, then replay logs only when safety_check against the watermark passes.
Conceptually:
Batching: “group multiple transactions into a Paxos log entry with a summary timestamp”.
Watermark: “monotone lower bound on safely replicated timestamps”, used for:
Deciding when it is safe to replay on followers (paper §4.4 progress-checking).
Deciding when a transaction is fully committed and can be acknowledged to clients (paper §4.4 & §4.5).
Transactions are not batched by the watermark; instead:
They are batched by StringAllocator based on entries / buffer size.
The watermark is updated after batches are replicated, and used as a safety gate for replay and client acks.
4. If you had to change the transaction engine, where would you do it?
There are two natural levels to change it, depending on how deep you want to go.
4.1 Swap out STO as the per-node engine (keep Mako’s distributed logic)
This is the cleanest conceptual boundary:
Primary adaptation point: mbta_wrapper and its indexes:
src/mako/benchmarks/mbta_wrapper_arena.hh (and mbta_wrapper.hh / mbta_wrapper_norm.hh):
mbta_wrapper::new_txn – currently calls Sto::start_transaction().
mbta_wrapper::commit_txn – currently calls Sto::commit().
mbta_wrapper::abort_txn / abort_txn_local – currently call Sto::abort().
mbta_ordered_index:
get/put/insert/remove call into MassTrans methods (transGet, transPut, etc.).
To plug in a new engine, you’d:
Implement a new transaction object / API (e.g., your own OCC or 2PL).
Replace the STO calls in mbta_wrapper_* with your engine’s start/commit/abort.
Replace or adapt mbta_ordered_index so that it routes get/put into your engine’s index/data structure instead of MassTrans.
As long as you preserve:
The abstract_db contract (new_txn/commit_txn/abort_txn/open_index),
And expose a per‑transaction commit timestamp that the logging layer can use,
the higher‑level Mako machinery (Paxos, watermark, failure recovery) can stay largely unchanged.
4.2 Change the lower-level OCC semantics but keep STO
If you want to keep the STO API but change its behavior (e.g., different validation/locking policy):
Work in:
src/mako/benchmarks/sto/Transaction.hh / .cc – transaction state machine, read/write sets, opacity checks.
src/mako/benchmarks/sto/MassTrans.hh – how reads/writes hit Masstree and how commit’s check() and install() work.
Optionally, the older Silo path in txn.h / txn_impl.h / txn_proto2_impl.h if you want to hook into that infrastructure instead.
Here you can, for example, implement:
Different lock acquisition order,
Per‑key locking instead of OCC,
Different read validation strategy, etc.
But you must still ensure:
A monotonic commit timestamp is available per transaction (what eventually goes into latest_commit_timestamp in StringAllocator).
Correctness relative to Masstree’s multi-version behavior.
4.3 If you change the timestamp model
If your new engine uses a different notion of commit order (e.g., Lamport clocks, HLC, something else), you also need to touch:
Where timestamps are attached to log batches:
StringAllocator::update_commit_id() and where latest_commit_timestamp is appended before add_log_to_nc.
Watermark computation:
sync_util::sync_logger’s assumptions that timestamps are scalar, comparable, and monotonically increasing.
Paxos callbacks:
Where they decode the batch timestamp from the log and update local_timestamp_[par_id].
The invariants from the paper you must keep:
If T₂ (transitively) depends on T₁, then TS(T₂) > TS(T₁).
The watermark is always a lower bound on all fully replicated commit timestamps at each shard.
Replay / client ack only when a transaction’s TS is ≤ the watermark (per shard, or vectorized if you reintroduce full vector clocks).
Short answer to “where would I change the transaction engine?”
High-level swap (recommended): replace STO by editing mbta_wrapper_* and mbta_ordered_index (and possibly mbta_sharded_ordered_index) to call your new engine instead of Sto::... / MassTrans.
Low-level behavior change: modify STO internals in benchmarks/sto/Transaction.* and MassTrans.hh.
Ensure your engine still provides:
A well‑defined commit order (timestamps),
Read/write sets (or equivalent) for correctness,
Hooks so the logging + watermark code can observe commit progress.
Okay what is the current implementation of occ, what type and where is it implemented. As far as I can see the occ code is just a simulation

In the current tree there are three different OCC-related code paths, and only one of them is the one you actually care about for Mako; the others are either an experiment or belong to the Janus/Deptran side.
1. The “simulation” you found
src/mako/vec/occ.cpp is exactly what it looks like: a hand‑rolled OCC microbenchmark to explore latency/throughput tradeoffs. It:
Manually builds readSet / writeSet vectors,
Does CAS on an integer “lock” field,
Sleeps to simulate remote access, then increments a “version” integer.
This file is not the OCC used by the Mako transaction engine; it’s an experiment.
2. The real OCC used by Mako’s storage engine
For Mako’s main path (through mbta_wrapper / Masstree), the OCC implementation comes from the STO transaction library, which is essentially a Silo‑style OCC engine.
The pieces are:
Record version + lock bits:
TransactionTid in benchmarks/sto/Interface.hh
It encodes per-record metadata:
threadid_mask, lock_bit, increment_value, etc.:
    class TransactionTid {    public:        typedef uint64_t type;        static constexpr type threadid_mask = type(0x1FF);        static constexpr type lock_bit      = type(0x200);        static constexpr type increment_value = type(0x4000);        // ...        static bool is_locked(type v);        static bool try_lock(type& v, int here);        static void inc_write_version(type& v);        static void unlock_write(type& v);        // ...    };
This is the Silo-style per-tuple TID: lock bit + owning thread id + monotonically increasing version.
Transaction object, read/write sets, validation:
Transaction in benchmarks/sto/Transaction.hh / Transaction.cc
This class:
Tracks a per-transaction set of TransItems (read and write entries).
On reads, it records (pointer to versioned_value, observed TID) into the transaction’s tset and, for “opacity”, may do extra checks (hard_check_opacity).
On commit, it:
Acquires locks on write-set items via TransactionTid::try_lock/lock_write.
Re-checks that all read-set items are still at the same version (or satisfy a predicate) via owner()->check() calls.
If validation succeeds, applies writes and bumps each record’s TransactionTid::type with inc_write_version.
On failure, aborts and releases locks; Transaction::stop(committed, writeset, nwriteset) handles lock release and cleanup.
Integration with Masstree:
MassTrans in benchmarks/sto/MassTrans.hh
This is the bridge between Transaction and Masstree’s versioned_value:
Reads:
    bool transGet(Str key, ValType& retval, threadinfo_type& ti = mythreadinfo) {        unlocked_cursor_type lp(table_, key);        bool found = lp.find_unlocked(*ti.ti);        if (found) {            versioned_value *e = lp.value();            auto item = t_read_only_item(e);   // attach to current Transaction            if (!validityCheck(item, e)) {                Sto::abort_without_throw();                TThread::transget_without_throw = true;                return false;            }            Version elem_vers;            if (!atomicRead(e, elem_vers, retval)) return false;            item.observe(tversion_type(elem_vers));  // record version for OCC            // ...        }        // ...    }
Writes:
    bool trans_write(...) {        // lookup or insert into Masstree, get versioned_value* e        auto item = t_item(e);        item.observe(tversion_type(v));  // remember version        item.add_write<...>(key, value); // stage write in txn’s write set        // commit path later locks and applies this    }
In other words, MassTrans attaches Masstree operations to the STO transaction, and STO provides OCC semantics (read-set, write-set, locking, validation).
Entry points Mako actually calls:
mbta_wrapper_arena.hh (or mbta_wrapper.hh in newer code):
  class mbta_wrapper : public abstract_db {  public:      void *new_txn(uint64_t txn_flags, str_arena &arena, void *buf, ...) {          Sto::start_transaction();          thr_arena = &arena;          return txn;  // txn is implicit via TThread::txn      }      bool commit_txn(void *txn) {          STD_OP(return Sto::commit());          return false;      }      void abort_txn(void *txn) {          try { Sto::abort(); } catch (Transaction::Abort E) {}      }  };
So for the path you hit via dbtest/mbta_wrapper, the actual OCC implementation is STO’s Silo‑style OCC: per-record TransactionTid with lock bit + thread id, transaction-local read/write sets, and commit-time validation + version bump.
3. Other OCC implementations in the tree
For completeness:
Janus/Deptran OCC (not Mako’s main engine):
src/memdb/txn_occ.h implements a classical OCC (TxnOCC) with:
version maps for read and write sets: ver_check_read_, ver_check_write_,
a version_check() method that is called at commit time.
src/deptran/occ/scheduler.cc wires this into the Janus scheduler:
    mdb_txn_mgr_ = make_shared<mdb::TxnMgrOCC>();    ((mdb::TxnOCC *) txn)->set_policy(mdb::OCC_LAZY);
That OCC is used when you run Deptran/Janus protocols via s_main, not when you use the Mako/mbta path.
src/mako/vec/occ.cpp is, as you suspected, a toy simulation/microbenchmark, not the production OCC engine.
4. Short direct answers
“What is the current implementation of OCC?”
For the Mako/mbta path it’s a Silo-style OCC implemented in the STO transaction library: Transaction + TransactionTid + MassTrans over Masstree.
“What type is it?”
Multi-version, commit-time OCC with per-record TID containing lock + thread id, read-set and write-set validation at commit (very close to Silo’s OCC).
“Where is it implemented?”
src/mako/benchmarks/sto/Interface.hh – TransactionTid, TThread metadata.
src/mako/benchmarks/sto/Transaction.hh and Transaction.cc – transaction state, read/write sets, commit/abort logic.
src/mako/benchmarks/sto/MassTrans.hh – Masstree integration.
Wired into Mako through mbta_wrapper_* and mbta_ordered_index (the adapter to abstract_db).
“Is the OCC code just a simulation?”
No: vec/occ.cpp is a simulation; the real OCC is the STO/Silo-style code above, which is what actually runs when you execute transactions through dbtest/mbta_wrapper.