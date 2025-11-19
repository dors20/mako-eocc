#include "txn_reordering/request_access_tracker.h"

namespace mako::txn_reordering {

void RequestAccessTracker::RecordRead(uint32_t req_nr, const std::string& key) {
  Ensure(req_nr).reads.push_back(key);
}

void RequestAccessTracker::RecordWrite(uint32_t req_nr, const std::string& key) {
  Ensure(req_nr).writes.push_back(key);
}

AccessInfo RequestAccessTracker::Consume(uint32_t req_nr) {
  auto it = data_.find(req_nr);
  if (it == data_.end()) {
    return {};
  }
  AccessInfo info = std::move(it->second);
  data_.erase(it);
  return info;
}

bool RequestAccessTracker::Empty() const {
  return data_.empty();
}

AccessInfo& RequestAccessTracker::Ensure(uint32_t req_nr) {
  return data_[req_nr];
}

}  // namespace mako::txn_reordering
