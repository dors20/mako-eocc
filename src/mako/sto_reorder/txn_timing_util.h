#pragma once

#include <chrono>

#include "occ_reorder/batch_validation_counters.h"
#include "occ_reorder/storage_reorder_counters.h"
#include "benchmarks/sto/Transaction.hh"

namespace mako {
namespace sto {

using TxnClock = std::chrono::steady_clock;

inline double DurationMicros(const TxnClock::duration& d) {
  return static_cast<double>(
      std::chrono::duration_cast<std::chrono::microseconds>(d).count());
}

inline bool HasTimestamp(const TxnClock::time_point& tp) {
  return tp.time_since_epoch().count() != 0;
}

inline void ResetTxnTiming(Transaction* txn) {
  if (!txn) {
    return;
  }
  txn->timing() = Transaction::TimingInfo{};
}

inline void RecordClientExec(Transaction* txn) {
  if (!txn) {
    return;
  }
  auto& timing = txn->timing();
  if (!HasTimestamp(timing.client_exec_done) && HasTimestamp(timing.client_start)) {
    timing.client_exec_done = TxnClock::now();
    occ::txn_client_exec_us_counter().offer(
        DurationMicros(timing.client_exec_done - timing.client_start));
  }
}

inline void RecordTxnCompletion(Transaction* txn, bool committed) {
  if (!txn) {
    return;
  }
  auto& timing = txn->timing();
  if (!HasTimestamp(timing.client_start)) {
    return;
  }
  const auto finish = TxnClock::now();
  const double delta = DurationMicros(finish - timing.client_start);
  if (committed) {
    occ::txn_total_latency_us_counter().offer(delta);
  } else {
    occ::txn_abort_latency_us_counter().offer(delta);
  }
}

inline void RecordStorageEnqueue(Transaction* txn) {
  if (!txn) {
    return;
  }
  auto& timing = txn->timing();
  if (!HasTimestamp(timing.storage_enqueue)) {
    timing.storage_enqueue = TxnClock::now();
  }
}

inline void RecordStorageDequeue(Transaction* txn) {
  if (!txn) {
    return;
  }
  auto& timing = txn->timing();
  if (!HasTimestamp(timing.storage_dequeue) && HasTimestamp(timing.storage_enqueue)) {
    timing.storage_dequeue = TxnClock::now();
    occ::storage_reorder_queue_wait_us_counter().offer(
        DurationMicros(timing.storage_dequeue - timing.storage_enqueue));
  }
}

inline void RecordValidatorEnqueue(Transaction* txn) {
  if (!txn) {
    return;
  }
  auto& timing = txn->timing();
  if (!HasTimestamp(timing.validator_enqueue)) {
    timing.validator_enqueue = TxnClock::now();
  }
}

inline void RecordValidatorDequeue(Transaction* txn) {
  if (!txn) {
    return;
  }
  auto& timing = txn->timing();
  if (!HasTimestamp(timing.validator_dequeue) && HasTimestamp(timing.validator_enqueue)) {
    timing.validator_dequeue = TxnClock::now();
    occ::validator_queue_wait_us_counter().offer(
        DurationMicros(timing.validator_dequeue - timing.validator_enqueue));
  }
}

}  // namespace sto
}  // namespace mako


