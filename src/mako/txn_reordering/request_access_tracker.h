#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace mako::txn_reordering {

struct AccessInfo {
  std::vector<std::string> reads;
  std::vector<std::string> writes;
};

class RequestAccessTracker {
 public:
  void RecordRead(uint32_t req_nr, const std::string& key);
  void RecordWrite(uint32_t req_nr, const std::string& key);
  AccessInfo Consume(uint32_t req_nr);
  bool Empty() const;

 private:
  AccessInfo& Ensure(uint32_t req_nr);

  std::unordered_map<uint32_t, AccessInfo> data_;
};

}  // namespace mako::txn_reordering
