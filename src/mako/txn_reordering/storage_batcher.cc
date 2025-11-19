#include "txn_reordering/storage_batcher.h"

#include <cstring>
#include <sstream>
#include <string>
#include <inttypes.h>
#include <chrono>

#include "counter.h"
#include "lib/common.h"
#include "lib/transport_request_handle.h"
#include "txn_reordering/request_access_tracker.h"
#include "txn_reordering/txn_reordering_config.h"
#include "util.h"

namespace mako::txn_reordering {
namespace {
constexpr uint32_t kDefaultBatchBytes = 64 * 1024;
event_counter evt_storage_direct_dispatch("trcc_storage_direct_dispatch");
event_counter evt_storage_batches("trcc_storage_batches");
event_avg_counter evt_storage_batch_size("trcc_storage_batch_size");
event_avg_counter evt_storage_reorder_us("trcc_storage_reorder_us");

std::string MakeCompositeKey(uint16_t table_id, const char* data, uint16_t len) {
  std::string composite;
  composite.reserve(len + 8);
  composite.append(std::to_string(table_id));
  composite.push_back(':');
  if (data != nullptr && len > 0) {
    composite.append(data, data + len);
  }
  return composite;
}
}  // namespace

StorageBatcher::StorageBatcher() {
  Reconfigure(TxnReorderingOptions::Instance().Get());
}

void StorageBatcher::Reconfigure(const TxnReorderingConfig& cfg) {
  enabled_ = cfg.enabled && cfg.storage.enabled;
  max_batch_ = cfg.storage.max_batch == 0 ? 1 : cfg.storage.max_batch;
  max_batch_bytes_ = cfg.storage.max_batch_bytes == 0
                         ? kDefaultBatchBytes
                         : cfg.storage.max_batch_bytes;
  flush_interval_us_ = cfg.storage.flush_interval_us;
  buffered_bytes_ = 0;
  buffer_.clear();
  first_enqueue_time_ = std::chrono::steady_clock::now();
}

void StorageBatcher::SetDispatcher(
    std::function<void(uint8_t, mako::TransportRequestHandle*, size_t)> fn) {
  dispatch_fn_ = std::move(fn);
}

void StorageBatcher::SetAccessTracker(RequestAccessTracker* tracker) {
  tracker_ = tracker;
}

void StorageBatcher::Enqueue(mako::TransportRequestHandle* handle,
                             size_t msg_size) {
  if (!handle) {
    Panic("StorageBatcher received null handle");
  }
  if (!dispatch_fn_) {
    Panic("StorageBatcher dispatcher not set");
  }

  if (tracker_ != nullptr) {
    CaptureAccess(handle->GetRequestType(), {handle->GetRequestType(), handle, msg_size});
  }

  if (!enabled_) {
    evt_storage_direct_dispatch.inc();
    PendingRequest request{handle->GetRequestType(), handle, msg_size};
    Execute(request);
    return;
  }

  if (buffer_.is_empty()) {
    first_enqueue_time_ = std::chrono::steady_clock::now();
  }

  buffer_.push(PendingRequest{handle->GetRequestType(), handle, msg_size});
  buffered_bytes_ += msg_size;

  if (buffer_.len() >= max_batch_ || buffered_bytes_ >= max_batch_bytes_) {
    Flush();
    return;
  }

  MaybeFlushByTime();
}

void StorageBatcher::Flush() {
  if (!enabled_) {
    buffer_.clear();
    buffered_bytes_ = 0;
    return;
  }
  if (buffer_.is_empty()) {
    return;
  }
  if (!dispatch_fn_) {
    Panic("StorageBatcher dispatcher not set");
  }

  const size_t batch_sz = buffer_.len();
  const uint64_t start_us = util::timer::cur_usec();
  evt_storage_batches.inc();
  evt_storage_batch_size.offer(batch_sz);
  const std::string before = DescribeSequence(buffer_);
  std::string after;
  after.reserve(before.size());

  const RequestClass passes[] = {
      RequestClass::kWrite, RequestClass::kRead, RequestClass::kNeutral};

  for (RequestClass pass : passes) {
    for (size_t i = 0; i < buffer_.len(); ++i) {
      PendingRequest& pending = buffer_[i];
      if (Classify(pending.req_type) == pass) {
        if (!after.empty()) {
          after.push_back(',');
        }
        after.append(RequestTypeName(pending.req_type));
        Execute(pending);
      }
    }
  }

  const uint64_t reorder_us = util::timer::cur_usec() - start_us;
  evt_storage_reorder_us.offer(reorder_us);

  Debug("storage_batcher reorder (batch=%zu, before=%s, after=%s, reorder_us=%" PRIu64 ")",
        batch_sz,
        before.c_str(),
        after.c_str(),
        reorder_us);

  buffer_.clear();
  buffered_bytes_ = 0;
}

void StorageBatcher::MaybeFlushByTime() {
  if (flush_interval_us_ == 0 || buffer_.is_empty()) {
    return;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - first_enqueue_time_);
  if (static_cast<uint32_t>(elapsed.count()) >= flush_interval_us_) {
    Flush();
  }
}

StorageBatcher::RequestClass StorageBatcher::Classify(uint8_t req_type) const {
  switch (req_type) {
    case batchLockReqType:
    case lockReqType:
    case installReqType:
      return RequestClass::kWrite;
    case getReqType:
    case scanReqType:
      return RequestClass::kRead;
    default:
      return RequestClass::kNeutral;
  }
}

void StorageBatcher::Execute(const PendingRequest& request) const {
  dispatch_fn_(request.req_type, request.handle, request.msg_size);
}

std::string StorageBatcher::DescribeSequence(
    const rusty::Vec<PendingRequest>& buffer) const {
  if (buffer.is_empty()) {
    return "[]";
  }
  std::ostringstream oss;
  for (size_t i = 0; i < buffer.len(); ++i) {
    if (i > 0) {
      oss << ",";
    }
    oss << RequestTypeName(buffer[i].req_type);
  }
  return oss.str();
}

const char* StorageBatcher::RequestTypeName(uint8_t req_type) const {
  switch (req_type) {
    case getReqType:
      return "get";
    case scanReqType:
      return "scan";
    case lockReqType:
      return "lock";
    case batchLockReqType:
      return "batchLock";
    case installReqType:
      return "install";
    case validateReqType:
      return "validate";
    default:
      return "other";
  }
}

uint32_t StorageBatcher::ExtractRequestId(
    uint8_t req_type, const mako::TransportRequestHandle& handle) const {
  auto& mutable_handle = const_cast<mako::TransportRequestHandle&>(handle);
  switch (req_type) {
    case getReqType:
      return reinterpret_cast<const get_request_t*>(mutable_handle.GetRequestBuffer())->req_nr;
    case scanReqType:
      return reinterpret_cast<const scan_request_t*>(mutable_handle.GetRequestBuffer())->req_nr;
    case lockReqType:
      return reinterpret_cast<const lock_request_t*>(mutable_handle.GetRequestBuffer())->req_nr;
    case batchLockReqType:
      return reinterpret_cast<const batch_lock_request_t*>(mutable_handle.GetRequestBuffer())->req_nr;
    default:
      return reinterpret_cast<const basic_request_t*>(mutable_handle.GetRequestBuffer())->req_nr;
  }
}

void StorageBatcher::CaptureAccess(
    uint8_t req_type, const PendingRequest& request) const {
  if (!tracker_) {
    return;
  }
  const uint32_t req_nr = ExtractRequestId(req_type, *request.handle);
  if (req_nr == 0) {
    return;
  }

  switch (req_type) {
    case getReqType: {
      auto* req = reinterpret_cast<get_request_t*>(request.handle->GetRequestBuffer());
      RecordRead(req_nr, req->table_id, req->key, req->len);
      break;
    }
    case scanReqType: {
      auto* req = reinterpret_cast<scan_request_t*>(request.handle->GetRequestBuffer());
      std::string key = "scan:" +
                        std::string(req->start_end_key, req->slen) + "|" +
                        std::string(req->start_end_key + req->slen, req->elen);
      tracker_->RecordRead(req_nr, key);
      break;
    }
    case lockReqType: {
      auto* req = reinterpret_cast<lock_request_t*>(request.handle->GetRequestBuffer());
      RecordWrite(req_nr, req->targert_server_id, req->key_and_value, req->klen);
      break;
    }
    case batchLockReqType: {
      auto* req = reinterpret_cast<batch_lock_request_t*>(request.handle->GetRequestBuffer());
      RecordBatchLock(tracker_, req_nr, reinterpret_cast<const char*>(req), req->batch_size);
      break;
    }
    default:
      break;
  }
}

void StorageBatcher::RecordRead(uint32_t req_nr,
                                uint16_t table_id,
                                const char* key,
                                uint16_t len) const {
  if (!tracker_ || key == nullptr || len == 0) {
    return;
  }
  tracker_->RecordRead(req_nr, MakeCompositeKey(table_id, key, len));
}

void StorageBatcher::RecordWrite(uint32_t req_nr,
                                 uint16_t table_id,
                                 const char* key,
                                 uint16_t len) const {
  if (!tracker_ || key == nullptr || len == 0) {
    return;
  }
  tracker_->RecordWrite(req_nr, MakeCompositeKey(table_id, key, len));
}

void StorageBatcher::RecordBatchLock(RequestAccessTracker* tracker,
                                     uint32_t req_nr,
                                     const char* data,
                                     uint16_t count) {
  if (!tracker || data == nullptr || count == 0) {
    return;
  }
  BatchLockRequestWrapper wrapper(const_cast<char*>(data));
  while (!wrapper.all_request_handled()) {
    char* key = nullptr;
    char* value = nullptr;
    uint16_t table = 0;
    uint16_t klen = 0;
    uint16_t vlen = 0;
    wrapper.read_one_request(&key, &klen, &value, &vlen, &table);
    tracker->RecordWrite(req_nr, MakeCompositeKey(table, key, klen));
  }
}

}  // namespace mako::txn_reordering
