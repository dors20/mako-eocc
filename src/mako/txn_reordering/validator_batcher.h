#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "rusty/vec.hpp"
#include "txn_reordering/request_access_tracker.h"
#include "txn_reordering/txn_reordering_config.h"

namespace mako {
class TransportRequestHandle;
}  // namespace mako

namespace mako::txn_reordering {

class ValidatorBatcher {
 public:
  ValidatorBatcher();

  void Reconfigure(const TxnReorderingConfig& cfg);
  void SetDispatcher(std::function<void(mako::TransportRequestHandle*, size_t)> fn);
  void SetAbortDispatcher(std::function<void(mako::TransportRequestHandle*)> fn);
  void SetAccessTracker(RequestAccessTracker* tracker);
  void Enqueue(mako::TransportRequestHandle* handle, size_t msg_size);
  void Flush();

  struct PendingValidation {
    mako::TransportRequestHandle* handle;
    size_t msg_size;
    uint32_t req_nr;
    std::vector<std::string> read_keys;
    std::vector<std::string> write_keys;
  };

 private:
  struct ReorderPlan {
    std::vector<int> commit_order;
    std::vector<int> aborts;
    size_t edge_count{0};
  };

  void ExecuteEntry(const PendingValidation& pending) const;
  void MaybeFlushByTime();
  std::string DescribeSequence(const rusty::Vec<PendingValidation>& buffer) const;
  ReorderPlan BuildReorderPlan(const std::vector<PendingValidation>& txns) const;
  bool FindCycle(const std::vector<std::vector<int>>& edges,
                 const std::vector<bool>& alive,
                 std::vector<int>* cycle) const;
  int ChooseVictim(const std::vector<std::vector<int>>& edges,
                   const std::vector<bool>& alive,
                   const std::vector<int>& cycle) const;
  bool SharesKey(const PendingValidation& lhs,
                 const PendingValidation& rhs) const;
  void ApplyAccessInfo(PendingValidation& pending);

  bool enabled_{false};
  uint32_t batch_size_{32};
  uint32_t max_wait_us_{0};
  ValidatorPolicyConfig policy_{};
  rusty::Vec<PendingValidation> buffer_;
  std::chrono::steady_clock::time_point first_enqueue_time_;
  std::function<void(mako::TransportRequestHandle*, size_t)> dispatch_fn_;
  std::function<void(mako::TransportRequestHandle*)> abort_fn_;
  RequestAccessTracker* tracker_{nullptr};
};

}  // namespace mako::txn_reordering
