#pragma once

#include "counter.h"

namespace mako {
namespace occ {

inline event_counter& storage_reorder_batches_counter() {
  static event_counter ctr("storage_reorder_batches");
  return ctr;
}

inline event_counter& storage_reorder_attempts_counter() {
  static event_counter ctr("storage_reorder_attempts");
  return ctr;
}

inline event_counter& storage_reorder_applied_counter() {
  static event_counter ctr("storage_reorder_applied");
  return ctr;
}

inline event_counter& storage_reorder_removed_counter() {
  static event_counter ctr("storage_reorder_removed_txns");
  return ctr;
}

inline event_counter& storage_reorder_cycles_counter() {
  static event_counter ctr("storage_reorder_cycles_detected");
  return ctr;
}

inline event_counter& storage_reorder_aborted_counter() {
  static event_counter ctr("storage_reorder_aborted_txns");
  return ctr;
}

inline event_avg_counter& storage_reorder_avg_batch_size_counter() {
  static event_avg_counter ctr("storage_reorder_avg_batch_size");
  return ctr;
}

inline event_avg_counter& storage_reorder_queue_wait_us_counter() {
  static event_avg_counter ctr("storage_reorder_queue_wait_us");
  return ctr;
}

inline event_avg_counter& storage_reorder_phase_build_us_counter() {
  static event_avg_counter ctr("storage_reorder_phase_build_us");
  return ctr;
}

inline event_avg_counter& storage_reorder_phase_reorder_us_counter() {
  static event_avg_counter ctr("storage_reorder_phase_reorder_us");
  return ctr;
}

inline event_avg_counter& storage_reorder_phase_finalize_us_counter() {
  static event_avg_counter ctr("storage_reorder_phase_finalize_us");
  return ctr;
}

}  // namespace occ
}  // namespace mako


