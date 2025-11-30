#pragma once

#include "counter.h"

namespace mako {
namespace occ {

inline event_counter& batch_validations_counter() {
  static event_counter ctr("batch_validations");
  return ctr;
}

inline event_counter& batch_validated_txns_counter() {
  static event_counter ctr("batch_validated_txns");
  return ctr;
}

inline event_counter& batch_aborted_txns_counter() {
  static event_counter ctr("batch_aborted_txns");
  return ctr;
}

inline event_avg_counter& batch_finalize_phase_us_counter() {
  static event_avg_counter ctr("batch_finalize_phase_us");
  return ctr;
}

inline event_avg_counter& avg_batch_size_counter() {
  static event_avg_counter ctr("avg_batch_size");
  return ctr;
}

inline event_avg_counter& avg_batch_validation_time_counter() {
  static event_avg_counter ctr("avg_batch_validation_time_us");
  return ctr;
}

inline event_counter& txn_reorder_attempts_counter() {
  static event_counter ctr("txn_reorder_attempts");
  return ctr;
}

inline event_counter& txn_reorder_applied_counter() {
  static event_counter ctr("txn_reorder_applied");
  return ctr;
}

inline event_counter& txn_reorder_removed_counter() {
  static event_counter ctr("txn_reorder_removed_txns");
  return ctr;
}

inline event_counter& txn_reorder_cycles_counter() {
  static event_counter ctr("txn_reorder_cycles_detected");
  return ctr;
}

inline event_avg_counter& sto_batch_phase_build_us_counter() {
  static event_avg_counter ctr("sto_batch_phase_build_us");
  return ctr;
}

inline event_avg_counter& sto_batch_phase_reorder_us_counter() {
  static event_avg_counter ctr("sto_batch_phase_reorder_us");
  return ctr;
}

inline event_avg_counter& sto_batch_phase_finalize_us_counter() {
  static event_avg_counter ctr("sto_batch_phase_finalize_us");
  return ctr;
}

inline event_avg_counter& validator_queue_wait_us_counter() {
  static event_avg_counter ctr("validator_queue_wait_us");
  return ctr;
}

inline event_avg_counter& txn_client_exec_us_counter() {
  static event_avg_counter ctr("txn_client_exec_us");
  return ctr;
}

inline event_avg_counter& txn_total_latency_us_counter() {
  static event_avg_counter ctr("txn_total_latency_us");
  return ctr;
}

inline event_avg_counter& txn_abort_latency_us_counter() {
  static event_avg_counter ctr("txn_abort_latency_us");
  return ctr;
}

}  // namespace occ
}  // namespace mako


