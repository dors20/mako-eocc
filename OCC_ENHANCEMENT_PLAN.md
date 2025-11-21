# OCC Enhancement Implementation Plan for Mako

## Goals Overview

1. **Analyze** current OCC implementation and identify bottlenecks
2. **Design** OCC enhancements (batching, parallel validation, reordering)
3. **Implement** efficient data structures and algorithms
4. **Evaluate** against baseline on TPC-C with varying contention

---

## Phase 1: Analysis of Current OCC Implementation

### 1.1 Performance Bottleneck Identification

**Files to Analyze:**
- `src/deptran/occ/scheduler.cc` - `DoPrepare()` method
- `src/memdb/txn_occ.cc` - `version_check()` method
- `src/deptran/occ/coordinator.h` - Transaction coordination

**Key Metrics to Measure:**
- Validation time per transaction
- Lock acquisition overhead
- Version checking time
- Memory allocation during validation

**Tools:**
- Profiling: `perf`, `gprof`, or `valgrind`
- Custom timing instrumentation
- Abort reason tracking

### 1.2 Abort Rate Measurement

**Test Scenarios:**
```cpp
// Add abort tracking
class AbortTracker {
    std::map<abort_reason, uint64_t> abort_counts_;
    std::map<txnid_t, uint32_t> txn_abort_counts_;
    
    void recordAbort(txnid_t txn_id, abort_reason reason);
    void printStatistics();
};
```

**Contention Levels:**
- Low: 1-2 concurrent transactions per key
- Medium: 5-10 concurrent transactions per key
- High: 20+ concurrent transactions per key
- Skewed: Zipfian distribution with hotspots

### 1.3 False Abort Analysis

**What to Track:**
- Aborts due to version conflicts that could be resolved by reordering
- Aborts due to read-write conflicts that are actually safe
- Aborts that occur because transactions validated in wrong order

**Implementation:**
```cpp
enum class AbortType {
    REAL_CONFLICT,      // Actual serializability violation
    FALSE_ABORT,        // Could be avoided with reordering
    VERSION_STALE,      // Read stale version unnecessarily
    ORDERING_ISSUE      // Wrong validation order
};

void analyzeAbort(txnid_t txn1, txnid_t txn2, AbortType type);
```

---

## Phase 2: Design OCC Enhancements

### 2.1 Early Abort Detection

**Goal**: Detect conflicts during execution, not just at validation

**Design:**
```cpp
class EarlyConflictDetector {
    // Track active transactions and their read/write sets
    std::unordered_map<txnid_t, ReadWriteSet> active_txns_;
    
    // Check conflicts when transaction performs operation
    bool checkConflict(txnid_t txn_id, const Operation& op);
    
    // Early abort if conflict detected
    void earlyAbort(txnid_t txn_id, abort_reason reason);
};
```

**Integration Points:**
- `TxnOCC::read_column()` - Check if read conflicts with active writes
- `TxnOCC::write_column()` - Check if write conflicts with active reads

### 2.2 Parallel Validation (VLDB 2019 Batching)

**Goal**: Batch transactions and validate in parallel using FVS algorithms

**Components:**

#### A. Batch Validator
```cpp
class BatchValidator {
    std::queue<txnid_t> pending_batch_;
    std::chrono::time_point batch_start_time_;
    size_t batch_size_threshold_;
    std::chrono::milliseconds batch_timeout_;
    
    void addTransaction(txnid_t txn_id);
    bool isBatchReady();
    ValidationBatch collectBatch();
};
```

#### B. Dependency Graph Builder
```cpp
class DependencyGraphBuilder {
    DependencyGraph buildGraph(const ValidationBatch& batch);
    
    // Build hash table of write sets
    std::unordered_map<Key, std::vector<txnid_t>> buildWriteMap(
        const ValidationBatch& batch
    );
    
    // Probe with read sets to find dependencies
    void addDependencies(
        DependencyGraph& graph,
        const ValidationBatch& batch,
        const WriteMap& write_map
    );
};
```

#### C. FVS Algorithms
```cpp
class FVSFinder {
    // SCC-based greedy
    std::set<txnid_t> sccBasedGreedy(
        DependencyGraph& graph,
        Policy policy
    );
    
    // Sort-based greedy
    std::set<txnid_t> sortBasedGreedy(
        DependencyGraph& graph,
        Policy policy,
        int k
    );
    
    // Hybrid (brute-force for small SCCs)
    std::set<txnid_t> hybridSccGreedy(
        DependencyGraph& graph,
        Policy policy,
        int threshold = 5
    );
};
```

### 2.3 Storage Layer Reordering (VLDB 2019)

**Goal**: Apply highest-version writes first, then process reads

**Design:**
```cpp
class StorageReordering {
    struct WriteRequest {
        txnid_t txn_id;
        Key key;
        Value value;
        version_t version;
    };
    
    // Buffer requests
    std::unordered_map<Key, std::vector<WriteRequest>> write_buffer_;
    std::vector<ReadRequest> read_buffer_;
    
    // Process batch: writes first (highest version), then reads
    void processBatch(const ValidationBatch& batch);
    
    // Apply highest-version write per key
    void applyWrites();
    
    // Process reads after writes applied
    void processReads();
};
```

### 2.4 Fine-Grained Dependency Tracking (BCC-style)

**Goal**: Track dependencies at column/field level, not just row level

**Current**: Row-level version checking
**Enhanced**: Column-level version checking

```cpp
class FineGrainedTracker {
    // Track column-level versions
    struct ColumnVersion {
        rowid_t row_id;
        colid_t col_id;
        version_t version;
    };
    
    std::unordered_map<row_column_pair, version_t> read_versions_;
    std::unordered_map<row_column_pair, version_t> write_versions_;
    
    // More precise conflict detection
    bool hasConflict(const ColumnVersion& read, const ColumnVersion& write);
};
```

### 2.5 Operation Reordering Within Transactions

**Goal**: Reorder reads/writes to reduce conflicts

**Design:**
```cpp
class OperationReordering {
    // Analyze transaction operations
    void analyzeTransaction(Transaction& txn);
    
    // Reorder to minimize conflicts
    void reorderOperations(Transaction& txn);
    
    // Heuristics:
    // - Read all keys before writing
    // - Group writes to same key together
    // - Delay writes to hot keys
};
```

### 2.6 Hybrid Approach (MOCC-style)

**Goal**: Use pessimistic locking for highly contended objects

**Design:**
```cpp
class HybridOCC {
    // Track contention per key
    std::unordered_map<Key, uint32_t> contention_count_;
    uint32_t contention_threshold_ = 10;
    
    // Decide: OCC or pessimistic locking?
    bool shouldUsePessimistic(const Key& key);
    
    // Switch to pessimistic for hot keys
    void acquireLock(const Key& key, LockType type);
};
```

---

## Phase 3: Implementation Plan

### 3.1 File Structure

```
src/deptran/occ/
├── scheduler.cc/h              # Modified: Add batching
├── coordinator.h                # Modified: Batch coordination
├── batch_validator.cc/h         # NEW: Batch collection
├── dependency_graph.cc/h        # NEW: Graph building
├── fvs_finder.cc/h              # NEW: FVS algorithms
├── storage_reordering.cc/h      # NEW: Storage layer reordering
├── early_detector.cc/h          # NEW: Early conflict detection
├── fine_grained_tracker.cc/h    # NEW: Column-level tracking
└── hybrid_occ.cc/h              # NEW: Hybrid OCC/pessimistic

src/memdb/
├── txn_occ.cc/h                 # Modified: Add early detection
└── versioned_row.cc/h            # Modified: Column-level versions
```

### 3.2 Implementation Order

#### Step 1: Analysis Infrastructure (Week 1)
- [ ] Add abort tracking and profiling
- [ ] Measure baseline performance
- [ ] Identify bottlenecks

#### Step 2: Batch Validator (Week 2)
- [ ] Implement `BatchValidator` class
- [ ] Add batch collection logic to `SchedulerOcc`
- [ ] Test batching with simple cases

#### Step 3: Dependency Graph (Week 3)
- [ ] Implement `DependencyGraph` data structure
- [ ] Implement `DependencyGraphBuilder`
- [ ] Test graph construction correctness

#### Step 4: FVS Algorithms (Week 4-5)
- [ ] Implement SCC-based greedy
- [ ] Implement sort-based greedy
- [ ] Implement hybrid algorithm
- [ ] Add policy functions (prod-degree, priority, etc.)

#### Step 5: Storage Reordering (Week 6)
- [ ] Implement `StorageReordering` class
- [ ] Integrate with batch processing
- [ ] Test reordering correctness

#### Step 6: Early Detection (Week 7)
- [ ] Implement `EarlyConflictDetector`
- [ ] Integrate with transaction execution
- [ ] Test early abort scenarios

#### Step 7: Fine-Grained Tracking (Week 8)
- [ ] Modify version checking to column-level
- [ ] Update `VersionedRow` class
- [ ] Test precision improvements

#### Step 8: Operation Reordering (Week 9)
- [ ] Implement operation analysis
- [ ] Add reordering heuristics
- [ ] Test conflict reduction

#### Step 9: Hybrid Approach (Week 10)
- [ ] Implement contention tracking
- [ ] Add pessimistic locking for hot keys
- [ ] Test hybrid behavior

#### Step 10: Integration & Testing (Week 11-12)
- [ ] Integrate all components
- [ ] End-to-end testing
- [ ] Performance tuning

---

## Phase 4: Evaluation Plan

### 4.1 Test Configurations

**Baseline:**
- Current Mako OCC (sequential validation)
- No batching
- Row-level version checking

**Enhanced:**
- Batch validation with FVS
- Storage reordering
- Early detection
- Fine-grained tracking
- Operation reordering
- Hybrid approach

### 4.2 Workloads

#### TPC-C Benchmark
```yaml
configurations:
  - name: "low_contention"
    warehouses: 10
    threads: 24
    zipf_theta: 0.0  # Uniform
    
  - name: "medium_contention"
    warehouses: 10
    threads: 48
    zipf_theta: 0.5
    
  - name: "high_contention"
    warehouses: 10
    threads: 96
    zipf_theta: 0.9  # Highly skewed
    
  - name: "hotspot"
    warehouses: 10
    threads: 96
    hotspot_keys: ["warehouse:1", "district:1:1"]
    hotspot_probability: 0.5
```

### 4.3 Metrics to Measure

#### Performance Metrics
- **Throughput**: Transactions per second
- **Latency**: P50, P95, P99 commit latency
- **Abort Rate**: Percentage of transactions aborted
- **False Abort Rate**: Aborts that could be avoided

#### Resource Metrics
- **CPU Usage**: Per-core utilization
- **Memory Usage**: Peak and average
- **Cache Misses**: L1/L2/L3 cache performance
- **Lock Contention**: Lock acquisition time

#### Correctness Metrics
- **Serializability**: Verify all committed transactions
- **Consistency**: Check database invariants
- **Recovery**: Test failure scenarios

### 4.4 Evaluation Scripts

```bash
#!/bin/bash
# evaluate_occ_enhancements.sh

# Run baseline
./build/dbtest --bench tpcc --config config/tpcc_baseline.yml \
    --runtime 60 --output baseline_results.json

# Run with batching
./build/dbtest --bench tpcc --config config/tpcc_batching.yml \
    --runtime 60 --output batching_results.json

# Run with all enhancements
./build/dbtest --bench tpcc --config config/tpcc_enhanced.yml \
    --runtime 60 --output enhanced_results.json

# Compare results
python scripts/compare_results.py \
    baseline_results.json \
    batching_results.json \
    enhanced_results.json
```

---

## Phase 5: Configuration Options

### 5.1 YAML Configuration

```yaml
occ:
  # Batching
  batch_validation:
    enabled: true
    batch_size: 100
    batch_timeout_ms: 10
    
  # FVS Algorithm
  fvs:
    algorithm: "hybrid"  # "scc", "sort", "hybrid"
    hybrid_threshold: 5
    sort_k_factor: 5
    policy: "prod_degree"  # "prod_degree", "priority", "abort_count"
    
  # Storage Reordering
  storage_reordering:
    enabled: true
    apply_writes_first: true
    
  # Early Detection
  early_detection:
    enabled: true
    check_on_read: true
    check_on_write: true
    
  # Fine-Grained Tracking
  fine_grained:
    enabled: true
    column_level: true
    
  # Operation Reordering
  operation_reordering:
    enabled: true
    read_before_write: true
    
  # Hybrid Approach
  hybrid:
    enabled: true
    contention_threshold: 10
    pessimistic_for_hot_keys: true
```

### 5.2 Runtime Configuration

```cpp
class OCCConfig {
    // Batching
    bool batch_validation_enabled_ = true;
    size_t batch_size_ = 100;
    std::chrono::milliseconds batch_timeout_{10};
    
    // FVS
    FVSAlgorithm fvs_algorithm_ = FVSAlgorithm::HYBRID;
    int hybrid_threshold_ = 5;
    int sort_k_factor_ = 5;
    PolicyType policy_ = PolicyType::PROD_DEGREE;
    
    // Features
    bool storage_reordering_enabled_ = true;
    bool early_detection_enabled_ = true;
    bool fine_grained_enabled_ = true;
    bool operation_reordering_enabled_ = true;
    bool hybrid_enabled_ = true;
};
```

---

## Phase 6: Key Implementation Details

### 6.1 Batch Validator Integration

**Modify `SchedulerOcc::DoPrepare()`:**

```cpp
bool SchedulerOcc::DoPrepare(txnid_t tx_id) {
    auto tx_box = dynamic_pointer_cast<TxOcc>(GetOrCreateTx(tx_id));
    auto txn = (mdb::TxnOCC*) get_mdb_txn(tx_id);
    
    if (batch_validator_enabled_) {
        // Add to batch instead of validating immediately
        batch_validator_->addTransaction(tx_id);
        
        // Check if batch is ready
        if (batch_validator_->isBatchReady()) {
            return processBatch();
        } else {
            // Wait for batch to fill
            return true;  // Defer validation
        }
    } else {
        // Original sequential validation
        return doSequentialValidation(tx_id);
    }
}

bool SchedulerOcc::processBatch() {
    // Collect batch
    auto batch = batch_validator_->collectBatch();
    
    // Build dependency graph
    auto graph = dependency_builder_->buildGraph(batch);
    
    // Find FVS
    auto fvs = fvs_finder_->findFVS(graph, policy_);
    
    // Abort FVS transactions
    for (auto txn_id : fvs) {
        abortTransaction(txn_id);
    }
    
    // Commit remaining in topological order
    auto commit_order = topologicalSort(graph.removeNodes(fvs));
    for (auto txn_id : commit_order) {
        commitTransaction(txn_id);
    }
    
    return true;
}
```

### 6.2 Dependency Graph Construction

```cpp
DependencyGraph DependencyGraphBuilder::buildGraph(
    const ValidationBatch& batch
) {
    DependencyGraph graph;
    
    // Step 1: Create nodes
    for (auto txn_id : batch.txn_ids) {
        graph.addNode(txn_id);
    }
    
    // Step 2: Build write map (hash table)
    std::unordered_map<Key, std::vector<txnid_t>> write_map;
    for (auto txn_id : batch.txn_ids) {
        auto txn = getTransaction(txn_id);
        for (auto key : txn->write_set) {
            write_map[key].push_back(txn_id);
        }
    }
    
    // Step 3: Probe with read sets
    for (auto txn_id : batch.txn_ids) {
        auto txn = getTransaction(txn_id);
        for (auto key : txn->read_set) {
            auto it = write_map.find(key);
            if (it != write_map.end()) {
                // Found conflict: txn_id reads key, other txns write it
                for (auto writer_id : it->second) {
                    if (writer_id != txn_id) {
                        // Add edge: reader → writer
                        // (reader must commit before writer)
                        graph.addEdge(txn_id, writer_id);
                    }
                }
            }
        }
    }
    
    return graph;
}
```

### 6.3 FVS Algorithm Selection

```cpp
std::set<txnid_t> FVSFinder::findFVS(
    DependencyGraph& graph,
    Policy policy
) {
    // Trim graph first
    graph.trim();
    
    // Choose algorithm based on graph size
    if (graph.nodeCount() < 20) {
        // Small: use hybrid with brute-force
        return hybridSccGreedy(graph, policy, threshold=5);
    } else if (graph.nodeCount() < 100) {
        // Medium: use SCC-based
        return sccBasedGreedy(graph, policy);
    } else {
        // Large: use sort-based for speed
        int k = std::max(1, graph.nodeCount() / 20);
        return sortBasedGreedy(graph, policy, k);
    }
}
```

---

## Phase 7: Success Criteria

### 7.1 Performance Targets

- **Abort Rate Reduction**: 30-50% reduction in false aborts
- **Throughput Improvement**: 20-40% increase in TPS
- **Latency**: P99 latency improvement of 10-20%
- **CPU Efficiency**: Similar or better CPU utilization

### 7.2 Correctness Requirements

- **Serializability**: All committed transactions maintain serializability
- **Consistency**: Database invariants preserved
- **No Regressions**: Baseline functionality unchanged

### 7.3 Scalability

- **Batch Size**: Handle batches of 100-1000 transactions
- **Concurrency**: Support 100+ concurrent transactions
- **Memory**: Overhead < 10% compared to baseline

---

## Phase 8: Testing Strategy

### 8.1 Unit Tests

- Batch validator correctness
- Dependency graph construction
- FVS algorithm correctness
- Storage reordering logic

### 8.2 Integration Tests

- End-to-end batch validation
- Early detection integration
- Fine-grained tracking accuracy

### 8.3 Performance Tests

- TPC-C with varying contention
- Skewed workloads
- Hotspot scenarios

### 8.4 Correctness Tests

- Serializability verification
- Consistency checks
- Failure recovery

---

## Timeline Summary

| Phase | Duration | Key Deliverables |
|-------|----------|------------------|
| **Analysis** | 1 week | Bottleneck report, baseline metrics |
| **Design** | 1 week | Architecture design, API specifications |
| **Implementation** | 10 weeks | All components implemented |
| **Testing** | 2 weeks | Unit/integration/performance tests |
| **Evaluation** | 2 weeks | Results analysis, paper/report |
| **Total** | **16 weeks** | Complete OCC enhancement system |

---

## Next Steps

1. **Start with Analysis**: Profile current OCC implementation
2. **Implement Batch Validator**: Foundation for all enhancements
3. **Add Dependency Graph**: Core data structure
4. **Implement FVS Algorithms**: Start with sort-based (simplest)
5. **Integrate and Test**: Iterative development

---

## References

- VLDB 2019 Paper: "Improving Optimistic Concurrency Control through Transaction Batching and Operation Reordering"
- Mako Codebase: `src/deptran/occ/`, `src/memdb/txn_occ.cc`
- Related Work: BCC, MOCC, Silo, TicToc

