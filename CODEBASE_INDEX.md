# Mako Codebase Index

This document provides a comprehensive index of the Mako distributed transactional key-value store codebase. It catalogs all major components, their purposes, relationships, and key implementation details.

## Table of Contents

1. [Project Overview](#project-overview)
2. [Directory Structure](#directory-structure)
3. [Core Components](#core-components)
4. [Transaction Protocols](#transaction-protocols)
5. [Storage Layer](#storage-layer)
6. [Networking & RPC](#networking--rpc)
7. [Replication & Consensus](#replication--consensus)
8. [Benchmarks](#benchmarks)
9. [Configuration System](#configuration-system)
10. [Build System](#build-system)
11. [Testing Infrastructure](#testing-infrastructure)

---

## Project Overview

**Mako** is a high-performance distributed transactional key-value store with geo-replication support, published at OSDI'25. Key innovations:

- **Speculative 2PC Protocol**: Decouples transaction execution from replication
- **Performance**: 3.66M TPC-C TPS with 10 shards, geo-replicated
- **Strong Consistency**: Serializable transactions with ACID guarantees
- **Fault Tolerance**: Multi-datacenter replication with Paxos consensus

### Key Technologies

- **Language**: C++17/23 with RustyCpp for memory safety
- **Storage**: Masstree (in-memory) + RocksDB (persistent)
- **Networking**: Custom RRR RPC framework + eRPC (RDMA support)
- **Consensus**: Multi-Paxos for replication
- **Build**: CMake (primary), Makefile, WAF (legacy)

---

## Directory Structure

### Root Level

```
mako/
├── src/              # Source code
├── build/            # Build artifacts
├── config/           # YAML configuration files
├── test/             # Test files
├── examples/         # Example programs
├── doc/              # Documentation
├── third-party/      # External dependencies
├── rust-lib/         # Rust components
├── bash/             # Deployment scripts
├── scripts/          # Utility scripts
├── pylib/            # Python utilities
└── ci/               # CI/CD scripts
```

### Source Directories

#### `src/mako/` - Core Mako System
- **Storage Engine**: Masstree-based B-tree implementation
- **Transaction Management**: Speculative 2PC protocol
- **Persistence**: RocksDB integration
- **Benchmarks**: TPC-C, TPC-A, micro-benchmarks

#### `src/deptran/` - Distributed Transaction Protocols
- **Janus**: Graph-based dependency tracking (OSDI'16)
- **2PL**: Two-phase locking
- **OCC**: Optimistic concurrency control
- **RCC/Rococo**: Distributed consensus protocol
- **Paxos**: Replication consensus
- **TAPIR**: Transaction protocol
- **Snow**: Additional protocol

#### `src/rrr/` - Custom RPC Framework
- **Reactor**: Event-driven coroutine system
- **RPC**: Client/server RPC implementation
- **Base**: Core utilities (logging, threading, marshaling)

#### `src/memdb/` - In-Memory Database
- Transaction abstractions for deptran protocols
- Schema management
- Row/table implementations

#### `src/bench/` - Benchmark Implementations
- TPC-C, TPC-A workloads
- Read-write benchmarks
- Micro-benchmarks

---

## Core Components

### Transaction System (`src/mako/txn.h`, `txn.cc`)

**Purpose**: Core transaction abstraction with speculative execution

**Key Classes**:
- `transaction_base`: Base transaction class with read/write sets
- `transaction<Protocol, Traits>`: Template-based transaction implementation
- `transaction_proto2`: Speculative 2PC protocol implementation

**Key Features**:
- Read set tracking (tuple → TID mapping)
- Write set tracking (tuple → value mapping)
- Absent set tracking (for scan consistency)
- Abort reason tracking
- RCU (Read-Copy-Update) support

**Transaction States**:
- `TXN_EMBRYO`: Allocated but no operations
- `TXN_ACTIVE`: Executing operations
- `TXN_COMMITED`: Successfully committed
- `TXN_ABRT`: Aborted

### Storage Engine (`src/mako/`)

**Masstree Integration**:
- `masstree_btree.h`: Masstree B-tree wrapper
- `txn_btree.cc/h`: Transaction-aware B-tree operations
- `tuple.cc/h`: Database tuple representation
- `btree.cc`: B-tree implementation

**Key Features**:
- Lock-free concurrent operations
- Multi-version concurrency control
- RCU-based garbage collection
- Optimized for NUMA architectures

### Core Infrastructure (`src/mako/core.h`, `core.cc`)

**Purpose**: Per-core data structures and thread management

**Key Classes**:
- `coreid`: Core ID allocation and management
- `percore<T>`: Per-core data structure template
- `percore_lazy<T>`: Lazy-initialized per-core structures

**Features**:
- Thread-local core ID assignment
- Cache-aligned data structures
- NUMA-aware allocation

### Memory Management (`src/mako/allocator.cc`, `memory.cc`)

**Allocators**:
- jemalloc (default)
- tcmalloc
- libc malloc
- Flow allocator

**Features**:
- Huge page support
- NUMA-aware allocation
- Memory pools for reduced overhead

---

## Transaction Protocols

### Mako Protocol (`src/mako/txn_proto2_impl.cc/h`)

**Purpose**: Speculative 2PC implementation for geo-replication

**Key Innovation**: Transactions commit locally before replication completes

**Phases**:
1. **Execute**: Read/write operations speculatively
2. **Prepare**: Check conflicts, assign dependencies
3. **Commit (Speculative)**: Apply writes, return success immediately
4. **Background Replication**: Asynchronously replicate via Paxos

**Dependency Tracking**:
- Builds conflict graph between concurrent transactions
- Prevents unbounded cascading aborts using watermarks
- Topological ordering for commit

**Watermark Mechanism**:
- Only transactions ≤ watermark are visible to reads
- Watermark advances when replication completes
- Ensures strong consistency despite speculation

### Janus Protocol (`src/deptran/janus/`)

**Purpose**: OSDI'16 protocol with graph-based dependency tracking

**Key Classes**:
- `CoordinatorJanus`: Coordinates distributed transactions
- `SchedulerJanus`: Executes transactions on shards
- `JanusCommo`: Communication layer

**Phases**:
- `DISPATCH`: Send transaction to shards
- `PRE_ACCEPT`: Fast path for conflict-free transactions
- `ACCEPT`: Slow path with full consensus
- `COMMIT`: Finalize transaction

### 2PL Protocol (`src/deptran/2pl/`)

**Purpose**: Traditional two-phase locking

**Key Classes**:
- `Coordinator2PL`: Coordinates with locking
- `Scheduler2PL`: Manages locks per shard

### OCC Protocol (`src/deptran/occ/`)

**Purpose**: Optimistic concurrency control

**Key Classes**:
- `CoordinatorOCC`: Coordinates with validation
- `SchedulerOCC`: Validates at commit time

### Paxos Protocol (`src/deptran/paxos/`)

**Purpose**: Multi-Paxos for replication consensus

**Key Classes**:
- `CoordinatorMultiPaxos`: Coordinates replication
- `PaxosServer`: Handles Paxos proposals
- `PaxosService`: Service interface

**Phases**:
- `PREPARE`: Leader election and proposal
- `ACCEPT`: Majority agreement
- `COMMIT`: Apply replicated value

---

## Storage Layer

### Masstree (`src/mako/masstree/`)

**Purpose**: High-performance in-memory B-tree index

**Features**:
- Lock-free concurrent operations
- Optimized for modern CPUs (cache-line aware)
- Supports variable-length keys (up to 1024 bytes)
- NUMA-aware design

**Integration**:
- Wrapped in `masstree_btree.h` for transaction support
- Used as primary index for all key-value operations

### RocksDB Persistence (`src/mako/rocksdb_persistence.cc/h`)

**Purpose**: Persistent storage backend

**Key Classes**:
- `RocksDBPersistence`: Singleton persistence manager
- `PartitionState`: Per-partition ordering state
- `PersistRequest`: Async persistence request

**Features**:
- Partitioned queues for parallel I/O
- Ordered callback execution per partition
- Sequence number tracking for ordering
- Epoch-based persistence

**API**:
```cpp
RocksDBPersistence::getInstance().persistAsync(
    data, size, shard_id, partition_id, callback);
```

### Tuple Management (`src/mako/tuple.cc/h`)

**Purpose**: Database tuple representation

**Key Classes**:
- `dbtuple`: Database tuple with TID (transaction ID)
- `tuple_writer_t`: Function pointer for value serialization

**Features**:
- Multi-version support
- Lock flags for write operations
- TID-based versioning

---

## Networking & RPC

### Transport Layer (`src/mako/lib/transport.h`, `transport.cc`)

**Purpose**: Abstract transport interface

**Key Classes**:
- `Transport`: Abstract transport interface
- `TransportReceiver`: Message receiver interface
- `Timeout`: Timer management

**Backends**:
1. **RRR RPC** (`src/mako/lib/rrr_rpc_backend.cc`): Default TCP/IP
2. **eRPC** (`src/mako/lib/erpc_backend.cc`): High-performance RDMA

**Switching Backends**:
```bash
# Use RRR RPC (default)
./build/dbtest config/mako_tpcc.yml

# Use eRPC
MAKO_TRANSPORT=erpc ./build/dbtest config/mako_tpcc.yml
```

### RRR Framework (`src/rrr/`)

**Purpose**: Custom RPC and event system

**Key Components**:

#### Reactor (`src/rrr/reactor/reactor.h`)
- Event-driven coroutine system
- Thread-local reactor instances
- Coroutine scheduling and management

#### RPC (`src/rrr/rpc/`)
- `client.hpp`: RPC client implementation
- `server.hpp`: RPC server implementation
- `utils.hpp`: RPC utilities

#### Base (`src/rrr/base/`)
- `threading.hpp`: Thread utilities
- `logging.hpp`: Logging system
- `marshal.hpp`: Message serialization

**Features**:
- Coroutine-based async I/O
- Epoll-based event loop
- Thread-safe operations
- RustyCpp integration for memory safety

### Client/Server (`src/mako/lib/`)

**Client** (`client.h`, `client.cc`):
- `Client`: RPC client for shard communication
- Methods: `InvokeGet`, `InvokeLock`, `InvokeBatchLock`, etc.
- Async request/response handling

**Server** (`server.h`, `server.cc`):
- `ShardReceiver`: Receives requests from clients
- `ShardServer`: Server instance per shard
- Handlers: `HandleGetRequest`, `HandleLockRequest`, etc.

**ShardClient** (`shardClient.h`, `shardClient.cc`):
- Client-side shard communication
- Batch operations support

---

## Replication & Consensus

### Paxos Implementation (`src/deptran/paxos/`)

**Purpose**: Multi-Paxos for geo-replication

**Key Components**:
- `CoordinatorMultiPaxos`: Coordinates replication
- `PaxosServer`: Handles proposals and votes
- `PaxosService`: Service interface

**Phases**:
1. **PREPARE**: Leader proposes value
2. **ACCEPT**: Followers vote
3. **COMMIT**: Apply committed value

**Optimizations**:
- Skip PREPARE after first proposal (Multi-Paxos)
- Batching for throughput
- Leader stability

### Replication Flow

```
Leader                    Follower 1          Follower 2
  │                          │                   │
  ├─PREPARE─────────────────→│                   │
  │                          ├─vote─────────────→│
  │←─────ACCEPT──────────────│                   │
  │                          │                   │
  ├─ACCEPT──────────────────→│                   │
  │                          ├─vote─────────────→│
  │←─────COMMIT──────────────│                   │
  │                          │                   │
  ├─COMMIT──────────────────→│                   │
  │                          │                   │
```

### Fault Tolerance

**Failure Scenarios**:
- **Leader Failure**: New leader election
- **Follower Failure**: Continue with majority
- **Network Partition**: Majority quorum required

**Quorum Calculation**:
- `N` replicas → tolerate `⌊N/2⌋` failures
- 3 replicas → tolerate 1 failure
- 5 replicas → tolerate 2 failures

---

## Benchmarks

### TPC-C (`src/mako/benchmarks/tpcc.cc`, `tpcc.h`)

**Purpose**: Industry-standard OLTP benchmark

**Features**:
- 5 transaction types (NewOrder, Payment, etc.)
- Configurable warehouses per shard
- Realistic workload distribution

### TPC-A (`src/bench/tpca/`)

**Purpose**: Simple debit/credit benchmark

### Read-Write Benchmarks (`src/bench/rw/`)

**Purpose**: Synthetic read/write workload

### Micro Benchmarks (`src/bench/micro/`)

**Purpose**: Low-level performance testing

### Benchmark Infrastructure (`src/mako/benchmarks/`)

**Key Files**:
- `dbtest.cc`: Main benchmark runner
- `abstract_db.h`: Database abstraction interface
- `bench.cc/h`: Benchmark framework
- `rpc_setup.cc/h`: RPC configuration

**Usage**:
```bash
./build/dbtest --bench tpcc --num-threads 24 --runtime 60 \
    --shard-config config/mako_tpcc.yml
```

---

## Configuration System

### Configuration Format (`src/mako/lib/configuration.h`, `configuration.cc`)

**Purpose**: YAML-based cluster configuration

**Old Format** (backward compatible):
```yaml
shards: 1
warehouses: 6
localhost:
  - name: shard0
    ip: 127.0.0.1
    port: 31000
p1:
  - name: shard0
    ip: 127.0.0.1
    port: 32000
```

**New Format** (recommended):
```yaml
sites:
    - name: "s0_leader"
      id: 1
      ip: 127.0.0.1
      port: 31000
    - name: "s0_follower1"
      ip: 127.0.0.1
      port: 32000

shard_map:
    - ["s0_leader", "s0_follower1"]  # Shard 0

warehouses: 6
```

**Key Classes**:
- `transport::Configuration`: Configuration parser
- `transport::ShardAddress`: Shard address (old format)
- `transport::SiteInfo`: Site information (new format)

**Features**:
- Automatic format detection
- Backward compatibility
- Multi-shard single-process support
- Transport backend configuration

### Configuration Files (`config/`)

**Examples**:
- `mako_single_node.yml`: Single node setup
- `mako_new_format.yml`: Multi-replica example
- `tpcc.yml`: TPC-C benchmark config
- `1leader_2followers/`: Replication test configs

---

## Build System

### CMake (`CMakeLists.txt`)

**Primary Build System**: CMake 3.10+

**Key Targets**:
- `mako`: Core library
- `txlog`: Shared library for deptran protocols
- `dbtest`: Main benchmark executable
- `simpleTransaction`: Simple transaction example
- `simplePaxos`: Paxos replication example

**Build Modes**:
- `perf`: Performance-optimized (default)
- `backoff`: Backoff mode
- `factor-gc`: GC factor mode
- `sandbox`: Sandbox mode

**Configuration Options**:
- `DEBUG`: Debug mode
- `CHECK_INVARIANTS`: Invariant checking
- `USE_MALLOC_MODE`: Allocator selection (0=libc, 1=jemalloc, 2=tcmalloc)
- `DISABLE_DISK`: Disable disk persistence

**Dependencies**:
- Masstree (submodule)
- RocksDB
- eRPC (submodule)
- RustyCpp (submodule)
- yaml-cpp
- Boost (system, filesystem, thread, coroutine, context)
- libevent
- jemalloc

### Makefile (`Makefile`)

**Purpose**: Convenience wrapper for CMake

**Targets**:
- `make build`: Configure and build
- `make test`: Run tests
- `make clean`: Clean build artifacts

### Rust Components (`rust-lib/`)

**Purpose**: Rust library for Redis-compatible interface

**Build**: Cargo-based, integrated into CMake

---

## Testing Infrastructure

### Test Files (`test/`)

**Unit Tests**:
- `test_marshal.cc`: Marshal/unmarshal tests
- `test_rpc.cc`: RPC client/server tests
- `test_reactor.cc`: Reactor/event system tests
- `test_future.cc`: Future/async operation tests
- `test_coroutine.cc`: Coroutine tests

**Integration Tests** (`ci/ci.sh`):
- `simpleTransaction`: Simple transaction test
- `simplePaxos`: Paxos replication test
- `shard1Replication`: 1-shard with replication
- `shard2Replication`: 2-shards with replication
- `shardFaultTolerance`: Fault tolerance test
- `multiShardSingleProcess`: Multi-shard mode test

### Example Programs (`examples/`)

- `simpleTransaction.cc`: Basic transaction example
- `simpleTransactionRep.cc`: Transaction with replication
- `simplePaxos.cc`: Paxos replication example
- `continuousTransactions.cc`: Continuous transaction stream
- `test_rocksdb_persistence.cc`: RocksDB persistence test

### CI/CD (`ci/ci.sh`)

**Purpose**: Automated test runner

**Usage**:
```bash
./ci/ci.sh all                    # Run all tests
./ci/ci.sh simpleTransaction      # Run specific test
./ci/ci.sh shard1Replication      # Replication test
```

---

## Key Design Patterns

### Speculative Execution

**Pattern**: Execute optimistically, handle failures asynchronously

**Implementation**:
1. Transaction executes locally
2. Returns success immediately
3. Replication happens in background
4. Watermark prevents visibility until replication completes

### Dependency Tracking

**Pattern**: Build conflict graph, topological ordering

**Implementation**:
- Track read/write sets
- Detect conflicts between transactions
- Build dependency graph
- Order commits topologically

### Watermark Mechanism

**Pattern**: Only expose stable transactions

**Implementation**:
- Watermark = highest replicated transaction
- Reads only see transactions ≤ watermark
- Watermark advances as replication completes

### RCU (Read-Copy-Update)

**Pattern**: Lock-free reads, copy-on-write updates

**Implementation**:
- Readers use RCU regions
- Writers create new versions
- Old versions garbage collected after grace period

---

## Memory Safety

### RustyCpp Integration

**Purpose**: Rust-like memory safety in C++

**Migrated Components**:
- Event system: `Cell<EventStatus>`
- Collections: `Vec` (aliased to `std::vector`)
- Smart pointers: `rusty::Box`, `rusty::Arc`, `rusty::Rc`

**Guidelines**:
- Use RustyCpp types for new code
- Avoid raw pointers
- Follow RAII patterns
- Document safety annotations

---

## Performance Optimizations

### NUMA Awareness

- Per-core data structures
- NUMA-aware memory allocation
- CPU affinity for threads

### Lock-Free Data Structures

- Masstree (lock-free B-tree)
- RCU for reads
- Atomic operations for coordination

### Batching

- Batch RPC requests
- Batch Paxos proposals
- Batch persistence writes

### Zero-Copy

- Message passing without copies
- Direct memory access where possible

---

## Documentation

### Main Documentation (`doc/`)

- `introduction.md`: Project overview
- `architecture.md`: System architecture
- `concepts.md`: Key concepts
- `config.md`: Configuration guide
- `install.md`: Installation instructions
- `transport_backends.md`: Transport layer documentation

### Code Documentation

- Inline comments in headers
- Doxygen-style documentation
- Architecture diagrams in `doc/`

---

## External Dependencies

### Submodules (`third-party/`)

- **Masstree**: High-performance B-tree
- **eRPC**: RDMA-based RPC framework
- **RustyCpp**: Memory safety checker
- **yaml-cpp**: YAML parsing

### System Libraries

- RocksDB: Persistent storage
- Boost: Coroutines, fibers, system
- libevent: Event loop
- jemalloc: Memory allocator
- DPDK: Kernel bypass (optional)

---

## Key Files Reference

### Core Transaction System
- `src/mako/txn.h`: Transaction base class
- `src/mako/txn_proto2_impl.cc/h`: Speculative 2PC implementation
- `src/mako/txn_btree.cc/h`: Transaction-aware B-tree

### Storage
- `src/mako/masstree_btree.h`: Masstree wrapper
- `src/mako/tuple.cc/h`: Tuple representation
- `src/mako/rocksdb_persistence.cc/h`: RocksDB persistence

### Networking
- `src/mako/lib/transport.h`: Transport interface
- `src/mako/lib/rrr_rpc_backend.cc`: RRR RPC backend
- `src/mako/lib/erpc_backend.cc`: eRPC backend
- `src/rrr/reactor/reactor.h`: Event reactor

### Protocols
- `src/deptran/janus/coordinator.h`: Janus coordinator
- `src/deptran/paxos/coordinator.h`: Paxos coordinator
- `src/deptran/coordinator.h`: Base coordinator

### Configuration
- `src/mako/lib/configuration.cc/h`: Configuration parser
- `config/*.yml`: Configuration files

### Benchmarks
- `src/mako/benchmarks/dbtest.cc`: Main benchmark runner
- `src/mako/benchmarks/tpcc.cc`: TPC-C benchmark

---

## Development Workflow

### Building

```bash
# Full build
make -j32

# Clean build
make clean && make -j32

# Debug build
cmake -DDEBUG=ON -B build
cmake --build build -j32
```

### Testing

```bash
# Run all tests
./ci/ci.sh all

# Run specific test
./ci/ci.sh simpleTransaction

# Run with CTest
cd build && ctest --output-on-failure
```

### Running Benchmarks

```bash
# TPC-C benchmark
./build/dbtest --bench tpcc --num-threads 24 \
    --shard-config config/tpcc.yml --runtime 60

# Single node
./build/dbtest --site-name local_s0 \
    --shard-config config/mako_single_node.yml \
    --bench tpcc --num-threads 6
```

---

## Summary

This codebase implements a sophisticated distributed transactional key-value store with:

1. **Speculative 2PC Protocol**: Decouples execution from replication
2. **Multiple Protocol Support**: Janus, 2PL, OCC, RCC, Paxos, TAPIR
3. **High-Performance Storage**: Masstree + RocksDB
4. **Flexible Networking**: RRR RPC + eRPC backends
5. **Strong Consistency**: Serializable transactions with ACID guarantees
6. **Fault Tolerance**: Multi-datacenter replication with Paxos
7. **Memory Safety**: RustyCpp integration for safer C++

The system is designed for high throughput (3.66M TPS) while maintaining strong consistency guarantees across geo-replicated datacenters.

