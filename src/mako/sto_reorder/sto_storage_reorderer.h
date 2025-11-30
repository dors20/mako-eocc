#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "occ_reorder/storage_reorder_counters.h"
#include "sto_txn_reorder_controller.h"
#include "txn_timing_util.h"
#include "sto_batch_validator.h"

namespace mako {
namespace sto {

class StoStorageReorderer {
 public:
  static StoStorageReorderer& Instance() {
    static StoStorageReorderer instance;
    return instance;
  }

  void Configure(size_t batch_size, size_t max_wait_us) {
    std::lock_guard<std::mutex> lk(mutex_);
    batch_size_ = std::max<size_t>(1, batch_size);
    max_wait_us_ = max_wait_us;
    enabled_ = true;
    pending_.clear();
    shutdown_ = false;
    first_enqueue_time_ = std::chrono::steady_clock::now();
    (void) occ::storage_reorder_batches_counter();
    (void) occ::storage_reorder_attempts_counter();
    (void) occ::storage_reorder_applied_counter();
    (void) occ::storage_reorder_removed_counter();
    (void) occ::storage_reorder_cycles_counter();
    (void) occ::storage_reorder_aborted_counter();
    (void) occ::storage_reorder_avg_batch_size_counter();
    if (!worker_running_) {
      worker_running_ = true;
      worker_ = std::thread([this]() { WorkerLoop(); });
    }
  }

  void Shutdown() {
    std::unique_lock<std::mutex> lk(mutex_);
    if (!worker_running_) {
      enabled_ = false;
      shutdown_ = true;
      pending_.clear();
      return;
    }
    enabled_ = false;
    shutdown_ = true;
    cv_.notify_all();
    lk.unlock();
    if (worker_.joinable()) {
      worker_.join();
    }
    lk.lock();
    worker_running_ = false;
    pending_.clear();
  }

  StoBatchValidator::Decision Process(Transaction* txn) {
    if (!enabled_ || txn == nullptr) {
      return StoBatchValidator::Instance().Process(txn);
    }

    Entry entry;
    entry.txn = txn;

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (shutdown_) {
        return StoBatchValidator::Instance().Process(txn);
      }
      if (pending_.empty()) {
        first_enqueue_time_ = std::chrono::steady_clock::now();
      }
      pending_.push_back(&entry);
      RecordStorageEnqueue(txn);
      cv_.notify_one();
    }

    std::unique_lock<std::mutex> entry_lock(entry.mutex);
    entry.cv.wait(entry_lock, [&entry]() { return entry.done; });
    return entry.decision;
  }

 private:
  struct Entry {
    Transaction* txn{nullptr};
    StoBatchValidator::Decision decision{StoBatchValidator::Decision::kBypass};
    bool done{false};
    std::mutex mutex;
    std::condition_variable cv;
  };

  StoStorageReorderer() = default;
  ~StoStorageReorderer() { Shutdown(); }

  void WorkerLoop() {
    std::vector<Entry*> ready;
    while (true) {
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&]() {
          return shutdown_ || !pending_.empty();
        });

        if (shutdown_ && pending_.empty()) {
          break;
        }

        if (pending_.empty()) {
          continue;
        }

        bool flush_now = pending_.size() >= batch_size_;
        bool timed_out = false;

        if (!flush_now) {
          if (max_wait_us_ == 0) {
            timed_out = true;
          } else {
            const auto deadline = first_enqueue_time_ +
                                  std::chrono::microseconds(max_wait_us_);
            const bool predicate = cv_.wait_until(
                lock,
                deadline,
                [&]() {
                  return shutdown_ || pending_.size() >= batch_size_;
                });
            timed_out = !predicate;
          }
        }

        if (shutdown_ && pending_.empty()) {
          break;
        }

        const bool should_flush =
            flush_now || timed_out || shutdown_;
        if (!should_flush) {
          continue;
        }

        ready.swap(pending_);
        pending_.clear();
        first_enqueue_time_ = std::chrono::steady_clock::now();
      }

      if (!ready.empty()) {
        Flush(ready);
        ready.clear();
      }
    }

    // Notify any remaining entries if shutting down
    for (Entry* entry : ready) {
      if (!entry) {
        continue;
      }
      {
        std::lock_guard<std::mutex> lock(entry->mutex);
        entry->decision = StoBatchValidator::Decision::kBypass;
        entry->done = true;
      }
      entry->cv.notify_one();
    }
  }

  void Flush(const std::vector<Entry*>& entries) {
    if (entries.empty()) {
      return;
    }

    using clock = std::chrono::steady_clock;
    auto to_us = [](const clock::duration& d) -> double {
      return static_cast<double>(
          std::chrono::duration_cast<std::chrono::microseconds>(d).count());
    };
    const auto start = clock::now();

    occ::storage_reorder_batches_counter().inc();
    occ::storage_reorder_avg_batch_size_counter().offer(entries.size());
    occ::storage_reorder_attempts_counter().inc();

    std::vector<Transaction*> txns;
    txns.reserve(entries.size());
    for (auto* entry : entries) {
      RecordStorageDequeue(entry->txn);
      txns.push_back(entry->txn);
    }
    const auto build_done = clock::now();
    occ::storage_reorder_phase_build_us_counter().offer(to_us(build_done - start));

    auto plan = StoTxnReorderController::Plan(txns);
    const auto reorder_done = clock::now();
    occ::storage_reorder_phase_reorder_us_counter().offer(to_us(reorder_done - build_done));
    if (plan.removed_nodes > 0) {
      occ::storage_reorder_removed_counter().inc(plan.removed_nodes);
    }
    if (plan.cycle_components > 0) {
      occ::storage_reorder_cycles_counter().inc(plan.cycle_components);
    }
    if (plan.applied) {
      occ::storage_reorder_applied_counter().inc();
    }

    std::unordered_map<Transaction*, Entry*> entry_map;
    entry_map.reserve(entries.size());
    for (auto* entry : entries) {
      entry_map.emplace(entry->txn, entry);
    }

    for (auto* txn : plan.aborted) {
      occ::storage_reorder_aborted_counter().inc();
      auto it = entry_map.find(txn);
      if (it == entry_map.end()) {
        continue;
      }
      {
        std::lock_guard<std::mutex> lock(it->second->mutex);
        it->second->decision = StoBatchValidator::Decision::kAborted;
        it->second->done = true;
      }
      it->second->cv.notify_one();
      entry_map.erase(it);
    }

    const bool has_order = !plan.ordered.empty();
    const auto& execution_order = has_order ? plan.ordered : txns;

    for (auto* txn : execution_order) {
      if (!txn) {
        continue;
      }
      auto it = entry_map.find(txn);
      if (it == entry_map.end()) {
        continue;
      }
      auto decision = StoBatchValidator::Instance().Process(txn);
      {
        std::lock_guard<std::mutex> lock(it->second->mutex);
        it->second->decision = decision;
        it->second->done = true;
      }
      it->second->cv.notify_one();
      entry_map.erase(it);
    }

    for (auto& kv : entry_map) {
      auto* entry = kv.second;
      auto decision = StoBatchValidator::Instance().Process(entry->txn);
      {
        std::lock_guard<std::mutex> lock(entry->mutex);
        entry->decision = decision;
        entry->done = true;
      }
      entry->cv.notify_one();
    }

    const auto finalize_done = clock::now();
    occ::storage_reorder_phase_finalize_us_counter().offer(to_us(finalize_done - reorder_done));
  }

  size_t batch_size_{4};
  size_t max_wait_us_{1000};
  bool enabled_{false};
  bool shutdown_{false};
  bool worker_running_{false};

  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<Entry*> pending_;
  std::chrono::steady_clock::time_point first_enqueue_time_{std::chrono::steady_clock::now()};
  std::thread worker_;
};

}  // namespace sto
}  // namespace mako


