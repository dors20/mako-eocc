#ifndef _NDB_TXN_OCC_BATCH_VALIDATION_H_
#define _NDB_TXN_OCC_BATCH_VALIDATION_H_

#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <unordered_set>
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#ifdef _OPENMP
#include <omp.h>
#endif
#include "occ_reorder/txn_batch_metadata.h"
#include "occ_reorder/txn_reorder_controller.h"
#include "txn.h"
#include "occ_reorder/batch_validation_counters.h"
#include "occ_reorder/batch_validation_trace.h"
#include "macros.h"
#include "thread.h"
#include "core.h"
#include "counter.h"
#include "tuple.h"
#include "masstree_btree.h"

namespace mako {

/**
 * Parallel Batch Validator for OCC transactions
 *
 * Batches multiple transactions and validates them in parallel to improve
 * throughput under contention. This reduces the serialization bottleneck
 * of sequential validation.
 *
 * Design:
 * - Transactions ready to commit are batched together
 * - Batch validation runs in parallel across multiple threads
 * - Only transactions that pass validation proceed to write phase
 * - Failed transactions abort and retry
 */
template <template <typename> class Protocol, typename Traits>
class BatchValidator {
public:
  typedef transaction<Protocol, Traits> txn_type;
  
  // Configuration
  static const size_t DEFAULT_BATCH_SIZE = 32;
  static const size_t DEFAULT_MAX_WAIT_US = 1000;  // 1ms max wait to fill batch
  static const size_t DEFAULT_NUM_VALIDATION_THREADS = 4;

  struct ValidationResult {
    txn_type *txn;
    bool valid;
    typename transaction_base::abort_reason reason;
    
    ValidationResult() : txn(nullptr), valid(false), reason(transaction_base::ABORT_REASON_NONE) {}
    ValidationResult(txn_type *t, bool v, typename transaction_base::abort_reason r)
      : txn(t), valid(v), reason(r) {}
  };

private:
  // Batch of transactions pending validation
  struct ValidationBatch {
    std::vector<txn_type*> txns;
    std::vector<ValidationResult> results;
    std::atomic<size_t> completed_count{0};
    std::atomic<size_t> validated_count{0};
    bool is_full() const { return txns.size() >= batch_size_; }
    
    size_t batch_size_;
    
    ValidationBatch(size_t bs) : batch_size_(bs) {
      txns.reserve(bs);
      results.reserve(bs);
    }
    
    void reset() {
      txns.clear();
      results.clear();
      completed_count.store(0);
      validated_count.store(0);
    }
  };

  // Per-thread validation context
  struct ValidationContext {
    ValidationBatch *current_batch;
    size_t thread_id;
    
    ValidationContext(size_t tid) : current_batch(nullptr), thread_id(tid) {}
  };

  // Configuration
  size_t batch_size_;
  size_t max_wait_us_;
  size_t num_validation_threads_;
  bool enabled_;
  bool txn_reorder_enabled_{false};

  // Batch management
  std::mutex batch_mutex_;
  std::unique_ptr<ValidationBatch> pending_batch_;
  std::condition_variable batch_cv_;
  std::atomic<bool> shutdown_{false};

  // Thread pool for parallel validation (reserved for future async work)
  std::vector<ValidationContext> thread_contexts_;
  
  // Statistics (using function-local statics to avoid template static member issues)
  static event_counter& get_batch_validations_counter() {
    return occ::batch_validations_counter();
  }
  static event_counter& get_batch_validated_txns_counter() {
    return occ::batch_validated_txns_counter();
  }
  static event_counter& get_batch_aborted_txns_counter() {
    return occ::batch_aborted_txns_counter();
  }
  static event_avg_counter& get_avg_batch_size_counter() {
    return occ::avg_batch_size_counter();
  }
  static event_avg_counter& get_avg_batch_validation_time_counter() {
    return occ::avg_batch_validation_time_counter();
  }
  static event_counter& get_txn_reorder_attempts_counter() {
    return occ::txn_reorder_attempts_counter();
  }
  static event_counter& get_txn_reorder_applied_counter() {
    return occ::txn_reorder_applied_counter();
  }
  static event_counter& get_txn_reorder_removed_counter() {
    return occ::txn_reorder_removed_counter();
  }
  static event_counter& get_txn_reorder_cycles_counter() {
    return occ::txn_reorder_cycles_counter();
  }

  static void EnsureCounterReporterRegistered();
  static void ReportCountersAtExit();
  static void PrintCounter(const char* name);

public:
  BatchValidator()
    : batch_size_(DEFAULT_BATCH_SIZE),
      max_wait_us_(DEFAULT_MAX_WAIT_US),
      num_validation_threads_(DEFAULT_NUM_VALIDATION_THREADS),
      enabled_(false),
      pending_batch_(nullptr) {
    (void) get_batch_validations_counter();
    (void) get_batch_validated_txns_counter();
    (void) get_batch_aborted_txns_counter();
    (void) get_avg_batch_size_counter();
    (void) get_avg_batch_validation_time_counter();
    (void) get_txn_reorder_attempts_counter();
    (void) get_txn_reorder_applied_counter();
    (void) get_txn_reorder_removed_counter();
    (void) get_txn_reorder_cycles_counter();
  }

  ~BatchValidator() {
    shutdown();
    ReportCountersAtExit();
  }

  /**
   * Initialize the batch validator with configuration
   */
  void Init(size_t batch_size = DEFAULT_BATCH_SIZE,
            size_t max_wait_us = DEFAULT_MAX_WAIT_US,
            size_t num_threads = DEFAULT_NUM_VALIDATION_THREADS) {
    batch_size_ = batch_size;
    max_wait_us_ = max_wait_us;

    // Derive validation thread count:
    // 1) MAKO_BATCH_VALIDATION_THREADS (explicit override)
    // 2) OMP_NUM_THREADS if OpenMP is enabled
    // 3) Fallback to num_threads argument (usually DEFAULT_NUM_VALIDATION_THREADS)
    size_t threads = num_threads;
    if (const char* env = std::getenv("MAKO_BATCH_VALIDATION_THREADS")) {
      char* end = nullptr;
      unsigned long v = std::strtoul(env, &end, 10);
      if (end != env && v > 0) {
        threads = static_cast<size_t>(v);
      }
    }
#ifdef _OPENMP
    if (threads == 0) {
      if (const char* omp_env = std::getenv("OMP_NUM_THREADS")) {
        char* end = nullptr;
        unsigned long v = std::strtoul(omp_env, &end, 10);
        if (end != omp_env && v > 0) {
          threads = static_cast<size_t>(v);
        }
      }
    }
#endif
    if (threads == 0) {
      threads = DEFAULT_NUM_VALIDATION_THREADS;
    }
    // Avoid spawning more validation threads than there are txns in a batch
    threads = std::max<size_t>(1, std::min(threads, batch_size_));
    num_validation_threads_ = threads;
    enabled_ = true;
    EnsureCounterReporterRegistered();
    const char* reorder_env = std::getenv("MAKO_ENABLE_TXN_REORDER");
    if (reorder_env) {
      std::string flag(reorder_env);
      txn_reorder_enabled_ = (flag == "1" || flag == "true" || flag == "TRUE");
    }
    
    // Initialize pending batch
    pending_batch_ = std::make_unique<ValidationBatch>(batch_size_);
  }

  /**
   * Shutdown the batch validator
   */
  void shutdown() {
    if (!enabled_)
      return;
      
    shutdown_.store(true);
    batch_cv_.notify_all();
    enabled_ = false;
  }

  /**
   * Add transaction to batch for parallel validation
   * Blocks until batch is validated and returns validation result
   * 
   * @param txn Transaction to add to batch
   * @return true if transaction was validated in batch (check txn->state for result), 
   *         false if should validate immediately (batch disabled or snapshot)
   */
  bool AddToBatch(txn_type *txn) {
    if (!enabled_ || txn->is_snapshot()) {
      // Snapshots don't need validation or batching disabled
      return false;
    }
    
    std::unique_lock<std::mutex> lock(batch_mutex_);
    
    if (!pending_batch_) {
      pending_batch_ = std::make_unique<ValidationBatch>(batch_size_);
    }
    
    // Store current transaction position in batch
    size_t txn_position = pending_batch_->txns.size();
    pending_batch_->txns.push_back(txn);
    
    // Check if batch is full or should validate
    bool should_validate = pending_batch_->is_full();
    
    if (!should_validate) {
      // Wait for batch to fill or timeout
      should_validate = batch_cv_.wait_for(lock, std::chrono::microseconds(max_wait_us_),
                            [this] { 
                              return (pending_batch_ && pending_batch_->is_full()) || shutdown_.load(); 
                            });
      
      // If timeout and batch has transactions, validate anyway
      if (!should_validate && pending_batch_ && !pending_batch_->txns.empty()) {
        should_validate = true;
      }
    }
    
    if (should_validate && pending_batch_ && !pending_batch_->txns.empty()) {
      // Move batch and validate in parallel
      auto batch = std::move(pending_batch_);
      pending_batch_ = std::make_unique<ValidationBatch>(batch_size_);
      lock.unlock();
      
      // Notify other waiting threads
      batch_cv_.notify_all();
      
      // Validate batch in parallel (waits for completion)
      validate_batch_parallel(*batch);
      
      // Check if this transaction passed validation
      INVARIANT(txn_position < batch->results.size());
      return batch->results[txn_position].valid;
    }
    
    // Transaction not yet validated (waiting in batch)
    return false;
  }

  /**
   * Validate a single transaction's read set
   * This is called in parallel by validation threads
   */
  static bool ValidateTransactionReadSet(txn_type *txn) {
    INVARIANT(txn != nullptr);
    
    // Extract read set and write tuples (similar to commit logic)
    typename txn_type::dbtuple_write_info_vec write_dbtuples;
    
    // Copy write tuples (if any) for conflict checking
    if (!txn->write_set.empty()) {
      typename txn_type::write_set_map::iterator it = txn->write_set.begin();
      typename txn_type::write_set_map::iterator it_end = txn->write_set.end();
      for (size_t pos = 0; it != it_end; ++it, ++pos) {
        write_dbtuples.emplace_back(it->get_tuple(), &(*it), it->is_insert(), pos);
      }
    }
    
    // Validate read set (same logic as txn_impl.h:343-368)
    if (!txn->read_set.empty()) {
      typename txn_type::read_set_map::iterator it = txn->read_set.begin();
      typename txn_type::read_set_map::iterator it_end = txn->read_set.end();
      for (; it != it_end; ++it) {
        // Check if tuple is in write set
        bool found = false;
        const dbtuple *read_tuple = it->get_tuple();
        for (const auto &w : write_dbtuples) {
          if (w.tuple.get() == read_tuple) {
            found = true;
            break;
          }
        }
        
        // Check if version is still valid
        if (likely(found ?
              it->get_tuple()->is_latest_version(it->get_tid()) :
              it->get_tuple()->stable_is_latest_version(it->get_tid())))
          continue;
        
        // Validation failed - version changed
        return false;
      }
    }
    
    // Validate absent set (btree versions)
    if (!txn->absent_set.empty()) {
      typename txn_type::absent_set_map::iterator it = txn->absent_set.begin();
      typename txn_type::absent_set_map::iterator it_end = txn->absent_set.end();
      for (; it != it_end; ++it) {
        const uint64_t v = concurrent_btree::ExtractVersionNumber(it->first);
        if (unlikely(v != it->second.version)) {
          return false;
        }
      }
    }
    
    return true;
  }

private:
  /**
   * Validate a batch of transactions in parallel
   * Uses OpenMP or std::execution::par for parallel validation
   */
  void validate_batch_parallel(ValidationBatch &batch) {
    if (batch.txns.empty())
      return;
    
    get_batch_validations_counter().inc();
    get_avg_batch_size_counter().offer(batch.txns.size());
    
    auto start_time = std::chrono::high_resolution_clock::now();
    batch.results.clear();
    batch.completed_count.store(0);
    batch.validated_count.store(0);
    if (BatchValidationTraceEnabled()) {
      std::fprintf(stderr,
                   "[batch_validation] validating batch size=%zu reorder=%s\n",
                   batch.txns.size(),
                   txn_reorder_enabled_ ? "on" : "off");
    }

    if (txn_reorder_enabled_) {
      get_txn_reorder_attempts_counter().inc();
      auto plan = occ::TxnReorderController<Protocol, Traits>::Plan(batch.txns);
      if (plan.cycle_components > 0) {
        get_txn_reorder_cycles_counter().inc(plan.cycle_components);
      }
      if (plan.removed_nodes > 0) {
        get_txn_reorder_removed_counter().inc(plan.removed_nodes);
      }
      if (plan.applied) {
        get_txn_reorder_applied_counter().inc();
        if (BatchValidationTraceEnabled()) {
          std::fprintf(stderr,
                       "[txn_reorder] plan graph_nodes=%zu edges=%zu removed=%zu "
                       "cycle_components=%zu applied=1\n",
                       plan.graph_nodes,
                       plan.graph_edges,
                       plan.removed_nodes,
                       plan.cycle_components);
        }
        std::unordered_map<uint64_t, size_t> index_map;
        index_map.reserve(batch.txns.size());
        batch.results.assign(batch.txns.size(), ValidationResult());
        for (size_t i = 0; i < batch.txns.size(); ++i) {
          batch.results[i].txn = batch.txns[i];
          index_map.emplace(reinterpret_cast<uint64_t>(batch.txns[i]), i);
        }

        auto place_result = [&](txn_type* txn,
                                bool valid,
                                transaction_base::abort_reason reason) {
          auto it =
              index_map.find(reinterpret_cast<uint64_t>(txn));
          if (it == index_map.end()) {
            return;
          }
          batch.results[it->second] = ValidationResult(txn, valid, reason);
        };

        for (auto* txn : plan.aborted) {
          place_result(txn, false, transaction_base::ABORT_REASON_USER);
          txn->state = transaction_base::TXN_ABRT;
          txn->reason = transaction_base::ABORT_REASON_USER;
          get_batch_aborted_txns_counter().inc();
        }

        for (auto* txn : plan.ordered) {
          bool valid = ValidateTransactionReadSet(txn);
          auto reason = valid ? transaction_base::ABORT_REASON_NONE
                              : transaction_base::ABORT_REASON_READ_NODE_INTEREFERENCE;
          place_result(txn, valid, reason);
          if (valid) {
            get_batch_validated_txns_counter().inc();
          } else {
            txn->state = transaction_base::TXN_ABRT;
            txn->reason = reason;
            get_batch_aborted_txns_counter().inc();
          }
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(
            end_time - start_time)
                               .count();
        get_avg_batch_validation_time_counter().offer(duration_us);
        return;
      }
      if (BatchValidationTraceEnabled()) {
        std::fprintf(stderr,
                     "[txn_reorder] plan graph_nodes=%zu edges=%zu removed=%zu "
                     "cycle_components=%zu applied=0\n",
                     plan.graph_nodes,
                     plan.graph_edges,
                     plan.removed_nodes,
                     plan.cycle_components);
      }
    }

    batch.results.resize(batch.txns.size());
    
    // Parallel validation: distribute across threads
    #ifdef _OPENMP
    #pragma omp parallel for num_threads(num_validation_threads_) schedule(dynamic, 1)
    #endif
    for (size_t i = 0; i < batch.txns.size(); ++i) {
      txn_type *txn = batch.txns[i];
      
      // Validate this transaction
      bool valid = ValidateTransactionReadSet(txn);
      
      batch.results[i] = ValidationResult(
        txn,
        valid,
        valid ? transaction_base::ABORT_REASON_NONE : 
                transaction_base::ABORT_REASON_READ_NODE_INTEREFERENCE);
      
      batch.validated_count.fetch_add(1, std::memory_order_release);
    }
    
    // Wait for all validations to complete (should already be done with OpenMP)
    while (batch.validated_count.load() < batch.txns.size()) {
      std::this_thread::yield();
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(
      end_time - start_time).count();
    get_avg_batch_validation_time_counter().offer(duration_us);
    
    // Process results - mark transactions as validated
    for (size_t i = 0; i < batch.results.size(); ++i) {
      if (batch.results[i].valid) {
        get_batch_validated_txns_counter().inc();
      } else {
        get_batch_aborted_txns_counter().inc();
        batch.txns[i]->state = transaction_base::TXN_ABRT;
        batch.txns[i]->reason = batch.results[i].reason;
      }
    }
  }

};

template <template <typename> class Protocol, typename Traits>
void BatchValidator<Protocol, Traits>::EnsureCounterReporterRegistered() {
#ifdef ENABLE_EVENT_COUNTERS
  static std::once_flag register_once;
  std::call_once(register_once, []() {
    std::atexit(&BatchValidator::ReportCountersAtExit);
  });
#endif
}

template <template <typename> class Protocol, typename Traits>
void BatchValidator<Protocol, Traits>::ReportCountersAtExit() {
#ifdef ENABLE_EVENT_COUNTERS
  static std::once_flag emit_once;
  std::call_once(emit_once, []() {
    std::fprintf(stderr, "--- batch_validation_counters ---\n");
    PrintCounter("batch_validations");
    PrintCounter("batch_validated_txns");
    PrintCounter("batch_aborted_txns");
    PrintCounter("avg_batch_size");
    PrintCounter("avg_batch_validation_time_us");
    std::fprintf(stderr, "--- txn_reorder_counters ---\n");
    PrintCounter("txn_reorder_attempts");
    PrintCounter("txn_reorder_applied");
    PrintCounter("txn_reorder_removed_txns");
    PrintCounter("txn_reorder_cycles_detected");
  });
#endif
}

template <template <typename> class Protocol, typename Traits>
void BatchValidator<Protocol, Traits>::PrintCounter(const char* name) {
#ifdef ENABLE_EVENT_COUNTERS
  counter_data data;
  if (!event_counter::stat(name, data)) {
    return;
  }
  if (data.type_ == counter_data::TYPE_COUNT) {
    std::fprintf(stderr,
                 "%s: count=%llu\n",
                 name,
                 static_cast<unsigned long long>(data.count_));
  } else {
    const double avg =
        data.count_ ? static_cast<double>(data.sum_) / static_cast<double>(data.count_) : 0.0;
    std::fprintf(stderr,
                 "%s: count=%llu, max=%llu, avg=%.2f\n",
                 name,
                 static_cast<unsigned long long>(data.count_),
                 static_cast<unsigned long long>(data.max_),
                 avg);
  }
#else
  (void) name;
#endif
}

} // namespace mako

// Global singleton instance for batch validator
// Access via GetBatchValidator() function
template <template <typename> class Protocol, typename Traits>
inline mako::BatchValidator<Protocol, Traits>&
GetBatchValidator() {
  static mako::BatchValidator<Protocol, Traits> g_batch_validator;
  return g_batch_validator;
}

#endif /* _NDB_TXN_OCC_BATCH_VALIDATION_H_ */

