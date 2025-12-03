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
    idle_wait_us_ = ParseIdleWaitEnv();
    low_watermark_ = ParseLowWatermarkEnv();
    reorder_min_size_ = ParseMinReorderSizeEnv();
    if (reorder_min_size_ < size_t{1}) {
      reorder_min_size_ = 1;
    }
    if (low_watermark_ > batch_size_) {
      low_watermark_ = batch_size_;
    }
    if (low_watermark_ < size_t{1}) {
      low_watermark_ = 1;
    }
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

        const size_t pending_size = pending_.size();
        bool flush_now = pending_size >= batch_size_;
        bool timed_out = false;
        const size_t wait_us = DetermineWaitUs(pending_size);

        if (!flush_now) {
          if (wait_us == 0) {
            timed_out = true;
          } else {
            const auto deadline = first_enqueue_time_ +
                                  std::chrono::microseconds(wait_us);
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

    std::vector<Transaction*> txns;
    txns.reserve(entries.size());
    for (auto* entry : entries) {
      RecordStorageDequeue(entry->txn);
      txns.push_back(entry->txn);
    }
    const auto build_done = clock::now();
    occ::storage_reorder_phase_build_us_counter().offer(to_us(build_done - start));
    auto reorder_done = build_done;
    bool had_reorder = false;

    std::vector<Entry*> survivors;
    std::vector<Transaction*> survivor_txns;
    bool reorder_applied = false;

    if (txns.size() >= reorder_min_size_) {
      occ::storage_reorder_batches_counter().inc();
      occ::storage_reorder_avg_batch_size_counter().offer(entries.size());
      occ::storage_reorder_attempts_counter().inc();

      auto plan = StoTxnReorderController::Plan(txns);
      reorder_done = clock::now();
      had_reorder = true;
      occ::storage_reorder_phase_reorder_us_counter().offer(to_us(reorder_done - build_done));
      if (plan.removed_nodes > 0) {
        occ::storage_reorder_removed_counter().inc(plan.removed_nodes);
      }
      if (plan.cycle_components > 0) {
        occ::storage_reorder_cycles_counter().inc(plan.cycle_components);
      }
      if (plan.applied) {
        occ::storage_reorder_applied_counter().inc();
        reorder_applied = true;
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

      survivors.reserve(entry_map.size());
      survivor_txns.reserve(entry_map.size());

      auto capture_entry = [&](Transaction* txn) {
        auto it = entry_map.find(txn);
        if (it == entry_map.end()) {
          return;
        }
        survivors.push_back(it->second);
        survivor_txns.push_back(it->second->txn);
        entry_map.erase(it);
      };

      for (auto* txn : execution_order) {
        capture_entry(txn);
      }
      for (auto& kv : entry_map) {
        survivors.push_back(kv.second);
        survivor_txns.push_back(kv.second->txn);
      }
      entry_map.clear();

    } else {
      survivors.reserve(entries.size());
      survivor_txns.reserve(entries.size());
      for (auto* entry : entries) {
        survivors.push_back(entry);
        survivor_txns.push_back(entry->txn);
      }
    }

    if (!survivors.empty()) {
      std::vector<StoBatchValidator::Decision> decisions;
      StoBatchValidator::Instance().ProcessBatch(survivor_txns, decisions);
      for (size_t i = 0; i < survivors.size(); ++i) {
        auto decision = (i < decisions.size())
                            ? decisions[i]
                            : StoBatchValidator::Decision::kBypass;
        auto* entry = survivors[i];
        {
          std::lock_guard<std::mutex> lock(entry->mutex);
          entry->decision = decision;
          entry->done = true;
        }
        entry->cv.notify_one();
      }
    }

    const auto finalize_done = clock::now();
    const auto finalize_delta = had_reorder ? (finalize_done - reorder_done)
                                            : (finalize_done - build_done);
    occ::storage_reorder_phase_finalize_us_counter().offer(to_us(finalize_delta));
  }

  size_t ParseIdleWaitEnv() const {
    const char* env = std::getenv("MAKO_STORAGE_REORDER_IDLE_WAIT_US");
    if (!env) {
      return 10;
    }
    char* end = nullptr;
    unsigned long long value = std::strtoull(env, &end, 10);
    if (end == env) {
      return 10;
    }
    return static_cast<size_t>(value);
  }

  size_t ParseLowWatermarkEnv() const {
    const char* env = std::getenv("MAKO_STORAGE_REORDER_LOW_WATERMARK");
    if (!env) {
      return 4;
    }
    char* end = nullptr;
    unsigned long long value = std::strtoull(env, &end, 10);
    if (end == env) {
      return 4;
    }
    return static_cast<size_t>(value);
  }

  size_t ParseMinReorderSizeEnv() const {
    const char* env = std::getenv("MAKO_STORAGE_REORDER_MIN_SIZE");
    if (!env) {
      return 2;
    }
    char* end = nullptr;
    unsigned long long value = std::strtoull(env, &end, 10);
    if (end == env) {
      return 2;
    }
    if (value == 0) {
      value = 1;
    }
    return static_cast<size_t>(value);
  }

  size_t DetermineWaitUs(size_t queue_depth) const {
    if (queue_depth >= batch_size_) {
      return 0;
    }
    if (max_wait_us_ == 0) {
      return 0;
    }
    if (queue_depth >= low_watermark_) {
      return max_wait_us_;
    }
    if (idle_wait_us_ == 0) {
      return 0;
    }
    return std::min(max_wait_us_, idle_wait_us_);
  }

  size_t batch_size_{4};
  size_t max_wait_us_{1000};
  size_t idle_wait_us_{50};
  size_t low_watermark_{4};
  size_t reorder_min_size_{2};
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


