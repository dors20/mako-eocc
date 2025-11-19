#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "rusty/vec.hpp"
#include "txn_reordering/request_access_tracker.h"
#include "txn_reordering/txn_reordering_config.h"

namespace mako {
class TransportRequestHandle;
}  // namespace mako

namespace mako::txn_reordering {

class StorageBatcher {
 public:
  StorageBatcher();

  void Reconfigure(const TxnReorderingConfig& cfg);
  void SetDispatcher(std::function<void(uint8_t,
                                        mako::TransportRequestHandle*,
                                        size_t)> fn);
  void SetAccessTracker(RequestAccessTracker* tracker);
  void Enqueue(mako::TransportRequestHandle* handle, size_t msg_size);
  void Flush();

 private:
  enum class RequestClass : uint8_t { kWrite = 0, kRead, kNeutral };

  struct PendingRequest {
    uint8_t req_type;
    mako::TransportRequestHandle* handle;
    size_t msg_size;
  };

  RequestClass Classify(uint8_t req_type) const;
  void Execute(const PendingRequest& request) const;
  std::string DescribeSequence(const rusty::Vec<PendingRequest>& buffer) const;
  const char* RequestTypeName(uint8_t req_type) const;
  uint32_t ExtractRequestId(uint8_t req_type,
                            const mako::TransportRequestHandle& handle) const;
  void CaptureAccess(uint8_t req_type, const PendingRequest& request) const;
  void RecordRead(uint32_t req_nr,
                  uint16_t table_id,
                  const char* key,
                  uint16_t len) const;
  void RecordWrite(uint32_t req_nr,
                   uint16_t table_id,
                   const char* key,
                   uint16_t len) const;
  static void RecordBatchLock(RequestAccessTracker* tracker,
                              uint32_t req_nr,
                              const char* data,
                              uint16_t count);

  bool enabled_{false};
  uint32_t max_batch_{32};
  uint32_t max_batch_bytes_{64 * 1024};
  uint32_t flush_interval_us_{50};
  std::chrono::steady_clock::time_point first_enqueue_time_;
  size_t buffered_bytes_{0};
  rusty::Vec<PendingRequest> buffer_;
  std::function<void(uint8_t, mako::TransportRequestHandle*, size_t)> dispatch_fn_;
  RequestAccessTracker* tracker_{nullptr};
  
  void MaybeFlushByTime();
};

}  // namespace mako::txn_reordering
