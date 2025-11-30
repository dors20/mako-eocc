#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include "occ_reorder/batch_validation_counters.h"
#include "occ_reorder/batch_validation_trace.h"
#include "occ_reorder/dependency_graph.h"
#include "occ_reorder/fvs_policies.h"
#include "sto_txn_batch_metadata.h"
#include "sto_txn_reorder_controller.h"
#include "txn_timing_util.h"

namespace mako {
namespace sto {

class StoBatchValidator {
 public:
  enum class Decision { kBypass, kValidated, kAborted };

  static StoBatchValidator& Instance() {
    static StoBatchValidator validator;
    return validator;
  }

  void Configure(size_t batch_size, size_t max_wait_us) {
    std::lock_guard<std::mutex> lk(batch_mutex_);
    batch_size_ = std::max<size_t>(1, batch_size);
    max_wait_us_ = max_wait_us;
    enabled_ = true;
    reorder_enabled_ = ParseReorderEnv();
    reorder_options_ = {};
    reorder_options_.fvs_policy = ParsePolicyEnv();
    if (BatchValidationTraceEnabled()) {
      std::fprintf(stderr,
                   "[batch_validation] sto configure batch_size=%zu max_wait_us=%zu\n",
                   batch_size_,
                   max_wait_us_);
    }
    EnsureReporterRegistered();
    if (!pending_batch_) {
      pending_batch_ = std::make_unique<ValidationBatch>(batch_size_);
    }
    shutdown_ = false;
    if (!worker_running_) {
      worker_running_ = true;
      worker_ = std::thread([this]() { WorkerLoop(); });
    }
  }

  void Shutdown() {
    std::unique_lock<std::mutex> lk(batch_mutex_);
    if (!worker_running_) {
      enabled_ = false;
      shutdown_ = true;
      pending_batch_.reset();
      return;
    }
    enabled_ = false;
    shutdown_ = true;
    batch_cv_.notify_all();
    lk.unlock();
    if (worker_.joinable()) {
      worker_.join();
    }
    lk.lock();
    worker_running_ = false;
    pending_batch_.reset();
  }

  Decision Process(Transaction* txn) {
    if (!enabled_ || !txn) {
      return Decision::kBypass;
    }

    ValidationEntry entry;
    entry.txn = txn;

    {
      std::lock_guard<std::mutex> lock(batch_mutex_);
      if (shutdown_) {
        return Decision::kBypass;
      }
      if (!pending_batch_) {
        pending_batch_ = std::make_unique<ValidationBatch>(batch_size_);
      }
      pending_batch_->add_entry(&entry);
      RecordValidatorEnqueue(txn);
      batch_cv_.notify_one();
    }

    std::unique_lock<std::mutex> entry_lock(entry.mutex);
    entry.cv.wait(entry_lock, [&entry]() { return entry.done; });
    return entry.decision;
  }

 private:
  struct ValidationEntry {
    Transaction* txn{nullptr};
    Decision decision{Decision::kBypass};
    bool done{false};
    std::mutex mutex;
    std::condition_variable cv;
  };

  struct ValidationBatch {
    explicit ValidationBatch(size_t reserve) {
      entries.reserve(reserve);
    }

    void add_entry(ValidationEntry* entry) {
      if (!has_first_enqueue_) {
        first_enqueue_ = std::chrono::steady_clock::now();
        has_first_enqueue_ = true;
      }
      entries.push_back(entry);
    }

    void reset() {
      entries.clear();
      has_first_enqueue_ = false;
    }

    bool empty() const { return entries.empty(); }

    std::chrono::steady_clock::time_point deadline(size_t wait_us) const {
      if (!has_first_enqueue_) {
        return std::chrono::steady_clock::now();
      }
      return first_enqueue_ + std::chrono::microseconds(wait_us);
    }

    std::vector<ValidationEntry*> entries;
   private:
    std::chrono::steady_clock::time_point first_enqueue_{};
    bool has_first_enqueue_{false};
  };

  StoBatchValidator() = default;
  ~StoBatchValidator() { Shutdown(); }

  bool ParseReorderEnv() const;
  occ::FvsPolicy ParsePolicyEnv() const;
  void EnsureReporterRegistered();
  static void PrintCounter(const char* name);
  void ValidateBatch(ValidationBatch& batch);
  void WorkerLoop();

  using ReorderController =
      occ::GenericTxnReorderController<Transaction,
                                       StoTxnBatchBuilder,
                                       occ::ParallelGraphBackend>;

  size_t batch_size_{32};
  size_t max_wait_us_{1000};
  bool enabled_{false};
  bool shutdown_{false};
  bool worker_running_{false};
  bool reorder_enabled_{false};
  typename ReorderController::Options reorder_options_{};

  std::mutex batch_mutex_;
  std::condition_variable batch_cv_;
  std::unique_ptr<ValidationBatch> pending_batch_;
  std::thread worker_;
};

inline bool StoBatchValidator::ParseReorderEnv() const {
  const char* env = std::getenv("MAKO_ENABLE_TXN_REORDER");
  if (!env) {
    return false;
  }
  std::string flag(env);
  std::transform(flag.begin(),
                 flag.end(),
                 flag.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return flag == "1" || flag == "true" || flag == "on";
}

inline occ::FvsPolicy StoBatchValidator::ParsePolicyEnv() const {
  const char* env = std::getenv("MAKO_TXN_REORDER_FVS");
  if (!env) {
    return occ::FvsPolicy::MIN_ID;
  }
  std::string policy(env);
  std::transform(policy.begin(),
                 policy.end(),
                 policy.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (policy == "prod" || policy == "degree" || policy == "degprod") {
    return occ::FvsPolicy::PROD_DEGREE;
  }
  return occ::FvsPolicy::MIN_ID;
}

inline void StoBatchValidator::EnsureReporterRegistered() {
#ifdef ENABLE_EVENT_COUNTERS
  static std::once_flag once;
  std::call_once(once, []() {
    std::atexit([]() {
      std::fprintf(stderr, "--- batch_validation_counters ---\n");
      PrintCounter("batch_validations");
      PrintCounter("batch_validated_txns");
      PrintCounter("batch_aborted_txns");
      PrintCounter("batch_finalize_phase_us");
      PrintCounter("avg_batch_size");
      PrintCounter("avg_batch_validation_time_us");
      PrintCounter("sto_batch_phase_build_us");
      PrintCounter("sto_batch_phase_reorder_us");
      PrintCounter("sto_batch_phase_finalize_us");
      std::fprintf(stderr, "--- txn_reorder_counters ---\n");
      PrintCounter("txn_reorder_attempts");
      PrintCounter("txn_reorder_applied");
      PrintCounter("txn_reorder_removed_txns");
      PrintCounter("txn_reorder_cycles_detected");
      std::fprintf(stderr, "--- storage_reorder_counters ---\n");
      PrintCounter("storage_reorder_batches");
      PrintCounter("storage_reorder_attempts");
      PrintCounter("storage_reorder_applied");
      PrintCounter("storage_reorder_removed_txns");
      PrintCounter("storage_reorder_cycles_detected");
      PrintCounter("storage_reorder_aborted_txns");
      PrintCounter("storage_reorder_avg_batch_size");
      PrintCounter("storage_reorder_phase_build_us");
      PrintCounter("storage_reorder_phase_reorder_us");
      PrintCounter("storage_reorder_phase_finalize_us");
    });
  });
#endif
}

inline void StoBatchValidator::PrintCounter(const char* name) {
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

inline void StoBatchValidator::ValidateBatch(ValidationBatch& batch) {
  if (batch.empty()) {
    return;
  }

  using clock = std::chrono::steady_clock;
  auto to_us = [](const clock::duration& d) -> double {
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(d).count());
  };
  const auto start = clock::now();

  occ::batch_validations_counter().inc();
  occ::avg_batch_size_counter().offer(batch.entries.size());

  std::vector<Transaction*> txns;
  txns.reserve(batch.entries.size());
  for (auto* entry : batch.entries) {
    txns.push_back(entry->txn);
  }
  const auto build_done = clock::now();
  occ::sto_batch_phase_build_us_counter().offer(to_us(build_done - start));

  auto record_finalize = [&](const clock::time_point& phase_begin) {
    const auto finalize_done = clock::now();
    const double finalize_us = to_us(finalize_done - phase_begin);
    occ::sto_batch_phase_finalize_us_counter().offer(finalize_us);
    occ::batch_finalize_phase_us_counter().offer(finalize_us);
    occ::avg_batch_validation_time_counter().offer(to_us(finalize_done - start));
  };

  if (!reorder_enabled_ || txns.size() < 2) {
    for (auto* entry : batch.entries) {
      if (!entry) {
        continue;
      }
      occ::batch_validated_txns_counter().inc();
      {
        std::lock_guard<std::mutex> lock(entry->mutex);
        entry->decision = Decision::kValidated;
        entry->done = true;
      }
      RecordValidatorDequeue(entry->txn);
      entry->cv.notify_one();
    }
    record_finalize(build_done);
    batch.reset();
    return;
  }

  occ::txn_reorder_attempts_counter().inc();
  auto plan = StoTxnReorderController::Plan(txns, reorder_options_);
  const auto reorder_done = clock::now();
  occ::sto_batch_phase_reorder_us_counter().offer(to_us(reorder_done - build_done));
  if (plan.cycle_components > 0) {
    occ::txn_reorder_cycles_counter().inc(plan.cycle_components);
  }
  if (plan.removed_nodes > 0) {
    occ::txn_reorder_removed_counter().inc(plan.removed_nodes);
  }
  if (plan.applied) {
    occ::txn_reorder_applied_counter().inc();
  }
  if (BatchValidationTraceEnabled()) {
    std::fprintf(stderr,
                 "[batch_validation] sto batch size=%zu reorder_applied=%d removed=%zu cycles=%zu\n",
                 batch.entries.size(),
                 plan.applied ? 1 : 0,
                 plan.removed_nodes,
                 plan.cycle_components);
  }

  std::unordered_set<Transaction*> aborted_set(plan.aborted.begin(), plan.aborted.end());

  for (auto* entry : batch.entries) {
    if (!entry) {
      continue;
    }
    Decision decision = Decision::kValidated;
    if (aborted_set.count(entry->txn)) {
      if (BatchValidationTraceEnabled()) {
        std::fprintf(stderr,
                     "[batch_validation] abort txn=%p due to reorder plan\n",
                     static_cast<void*>(entry->txn));
      }
      occ::batch_aborted_txns_counter().inc();
      decision = Decision::kAborted;
    } else {
      occ::batch_validated_txns_counter().inc();
    }
    {
      std::lock_guard<std::mutex> lock(entry->mutex);
      entry->decision = decision;
      entry->done = true;
    }
    RecordValidatorDequeue(entry->txn);
    entry->cv.notify_one();
  }

  record_finalize(reorder_done);
  batch.reset();
}

inline void StoBatchValidator::WorkerLoop() {
  std::unique_ptr<ValidationBatch> ready_batch;
  while (true) {
    {
      std::unique_lock<std::mutex> lock(batch_mutex_);
      batch_cv_.wait(lock, [&]() {
        return shutdown_ || (pending_batch_ && !pending_batch_->empty());
      });

      if (shutdown_ && (!pending_batch_ || pending_batch_->empty())) {
        break;
      }

      if (!pending_batch_ || pending_batch_->empty()) {
        continue;
      }

      bool flush_now = pending_batch_->entries.size() >= batch_size_;
      bool timed_out = false;

      if (!flush_now) {
        if (max_wait_us_ == 0) {
          timed_out = true;
        } else {
          const auto deadline = pending_batch_->deadline(max_wait_us_);
          const bool predicate_met = batch_cv_.wait_until(
              lock,
              deadline,
              [&]() {
                return shutdown_ ||
                       (pending_batch_ &&
                        pending_batch_->entries.size() >= batch_size_);
              });
          timed_out = !predicate_met;
          flush_now = shutdown_ ||
                      (pending_batch_ &&
                       pending_batch_->entries.size() >= batch_size_);
        }
      }

      if (shutdown_ && (!pending_batch_ || pending_batch_->empty())) {
        break;
      }

      const bool should_flush = flush_now || timed_out || shutdown_;
      if (!should_flush) {
        continue;
      }

      if (pending_batch_ && !pending_batch_->empty()) {
        ready_batch = std::move(pending_batch_);
        pending_batch_ = std::make_unique<ValidationBatch>(batch_size_);
      }
    }

    if (ready_batch) {
      ValidateBatch(*ready_batch);
      ready_batch.reset();
    }
  }
}

}  // namespace sto
}  // namespace mako


