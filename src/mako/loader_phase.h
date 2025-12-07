// Lightweight thread-local flag to distinguish load-phase threads from
// steady-state benchmark worker threads.
//
// We use this to *disable OCC batch validation during the TPCC load phase*
// while keeping it enabled for the steady-state run. This avoids deadlocks
// in dbtest's multi-loader setup while still exercising real batch
// validation for the benchmark proper.

#pragma once

namespace mako {

// True when the current thread is running a TPCC loader (data loading phase).
// Defined in thread.cc as a single process-wide TLS variable.
extern __thread bool g_in_loader_phase;

}  // namespace mako


