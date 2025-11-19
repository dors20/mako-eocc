#include "txn_reordering/validator_batcher.h"

#include <algorithm>
#include <queue>
#include <sstream>
#include <string>
#include <vector>
#include <inttypes.h>
#include <unordered_set>

#include "counter.h"
#include "lib/common.h"
#include "lib/transport_request_handle.h"
#include "util.h"

namespace mako::txn_reordering {
namespace {
event_counter evt_validator_batches("trcc_validator_batches");
event_avg_counter evt_validator_batch_size("trcc_validator_batch_size");
event_counter evt_validator_aborts("trcc_validator_aborts");
event_counter evt_validator_dropped("trcc_validator_dropped");
event_avg_counter evt_validator_reorder_us("trcc_validator_reorder_us");
event_avg_counter evt_validator_conflicts("trcc_validator_conflicts");
uint32_t ExtractReqNr(mako::TransportRequestHandle* handle) {
  auto* req = reinterpret_cast<basic_request_t*>(handle->GetRequestBuffer());
  return req->req_nr;
}

// Returns true if any key in 'needles' is present in 'haystack'
bool SharesAny(const std::vector<std::string>& needles,
               const std::vector<std::string>& haystack) {
  for (const auto& key : needles) {
    if (std::find(haystack.begin(), haystack.end(), key) != haystack.end()) {
      return true;
    }
  }
  return false;
}
}  // namespace

ValidatorBatcher::ValidatorBatcher() {
  Reconfigure(TxnReorderingOptions::Instance().Get());
}

void ValidatorBatcher::Reconfigure(const TxnReorderingConfig& cfg) {
  enabled_ = cfg.enabled && cfg.validator.enabled;
  batch_size_ = cfg.validator.batch_size == 0 ? 1 : cfg.validator.batch_size;
  max_wait_us_ = cfg.validator.max_wait_us;
  policy_ = cfg.validator.policy;
  buffer_.clear();
  first_enqueue_time_ = std::chrono::steady_clock::now();
}

void ValidatorBatcher::SetDispatcher(
    std::function<void(mako::TransportRequestHandle*, size_t)> fn) {
  dispatch_fn_ = std::move(fn);
}

void ValidatorBatcher::SetAbortDispatcher(
    std::function<void(mako::TransportRequestHandle*)> fn) {
  abort_fn_ = std::move(fn);
}

void ValidatorBatcher::SetAccessTracker(RequestAccessTracker* tracker) {
  tracker_ = tracker;
}

void ValidatorBatcher::Enqueue(mako::TransportRequestHandle* handle,
                               size_t msg_size) {
  if (!handle) {
    Panic("ValidatorBatcher received null handle");
  }
  if (!dispatch_fn_) {
    Panic("ValidatorBatcher dispatcher not set");
  }

  PendingValidation pending{handle, msg_size, ExtractReqNr(handle)};
  ApplyAccessInfo(pending);

  if (!enabled_) {
    ExecuteEntry(pending);
    return;
  }

  if (buffer_.is_empty()) {
    first_enqueue_time_ = std::chrono::steady_clock::now();
  }

  buffer_.push(pending);

  if (buffer_.len() >= batch_size_) {
    Flush();
    return;
  }

  MaybeFlushByTime();
}

void ValidatorBatcher::Flush() {
  if (buffer_.is_empty()) {
    return;
  }
  if (!dispatch_fn_) {
    Panic("ValidatorBatcher dispatcher not set");
  }

  const uint64_t start_us = util::timer::cur_usec();

  std::vector<PendingValidation> ordered;
  ordered.reserve(buffer_.len());
  for (size_t i = 0; i < buffer_.len(); ++i) {
    ordered.push_back(buffer_[i]);
  }

  evt_validator_batches.inc();
  evt_validator_batch_size.offer(ordered.size());
  const std::string before = DescribeSequence(buffer_);
  const ReorderPlan plan = BuildReorderPlan(ordered);
  evt_validator_conflicts.offer(plan.edge_count);

  std::string after;
  for (size_t i = 0; i < plan.commit_order.size(); ++i) {
    if (i > 0) {
      after.push_back(',');
    }
    after.append(std::to_string(ordered[plan.commit_order[i]].req_nr));
    ExecuteEntry(ordered[plan.commit_order[i]]);
  }

  if (!plan.aborts.empty()) {
    evt_validator_aborts.inc(plan.aborts.size());
    evt_validator_dropped.inc(plan.aborts.size());
    if (!after.empty()) {
      after.push_back('|');
    }
    after.append("drop:");
    for (size_t i = 0; i < plan.aborts.size(); ++i) {
      if (i > 0) {
        after.push_back(',');
      }
      after.append(std::to_string(ordered[plan.aborts[i]].req_nr));
      if (abort_fn_) {
        abort_fn_(ordered[plan.aborts[i]].handle);
      } else {
        ExecuteEntry(ordered[plan.aborts[i]]);
      }
    }
  }

  const uint64_t reorder_us = util::timer::cur_usec() - start_us;
  evt_validator_reorder_us.offer(reorder_us);

  Debug("validator_batcher reorder (before=%s, after=%s, reorder_us=%" PRIu64 ")",
        before.c_str(),
        after.c_str(),
        reorder_us);

  buffer_.clear();
}

void ValidatorBatcher::ExecuteEntry(const PendingValidation& pending) const {
  dispatch_fn_(pending.handle, pending.msg_size);
}

void ValidatorBatcher::MaybeFlushByTime() {
  if (max_wait_us_ == 0 || buffer_.is_empty()) {
    return;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - first_enqueue_time_);
  if (static_cast<uint32_t>(elapsed.count()) >= max_wait_us_) {
    Flush();
  }
}

std::string ValidatorBatcher::DescribeSequence(
    const rusty::Vec<PendingValidation>& buffer) const {
  if (buffer.is_empty()) {
    return "[]";
  }
  std::ostringstream oss;
  for (size_t i = 0; i < buffer.len(); ++i) {
    if (i > 0) {
      oss << ",";
    }
    oss << buffer[i].req_nr;
  }
  return oss.str();
}

ValidatorBatcher::ReorderPlan ValidatorBatcher::BuildReorderPlan(
    const std::vector<PendingValidation>& txns) const {
  const size_t n = txns.size();
  std::vector<std::vector<int>> edges(n);
  size_t edge_count = 0;

  // Build DIRECTED conflict graph
  // Edge i -> j means i must execute before j
  // Rule: If Ti reads X and Tj writes X, then Ti -> Tj
  // We also check Write-Read dependency but usually validation implies Reader -> Writer
  // for validity of the read set.
  
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = 0; j < n; ++j) {
      if (i == j) continue;
      
      // Check RW dependency: i reads, j writes
      if (SharesAny(txns[i].read_keys, txns[j].write_keys)) {
        edges[i].push_back(static_cast<int>(j));
        edge_count++;
      }
    }
  }

  std::vector<bool> alive(n, true);
  std::vector<int> aborts;
  std::vector<int> cycle;

  while (FindCycle(edges, alive, &cycle)) {
    const int victim = ChooseVictim(edges, alive, cycle);
    if (victim < 0) {
      break;
    }
    alive[victim] = false;
    aborts.push_back(victim);
  }

  std::vector<int> in_degree(n, 0);
  for (size_t i = 0; i < n; ++i) {
    if (!alive[i]) {
      continue;
    }
    for (int j : edges[i]) {
      if (alive[j]) {
        ++in_degree[j];
      }
    }
  }

  std::queue<int> q;
  for (size_t i = 0; i < n; ++i) {
    if (alive[i] && in_degree[i] == 0) {
      q.push(static_cast<int>(i));
    }
  }

  std::vector<int> order;
  while (!q.empty()) {
    int node = q.front();
    q.pop();
    order.push_back(node);
    for (int next : edges[node]) {
      if (!alive[next]) {
        continue;
      }
      if (--in_degree[next] == 0) {
        q.push(next);
      }
    }
  }

  // If the graph still has nodes not scheduled (e.g., all removed), append them.
  // Note: with cycle detection, this loop handles disconnected components or nodes with no incoming edges.
  // But wait, if we broke all cycles, 'order' should contain all 'alive' nodes.
  // Just in case, add any remaining alive nodes (should happen only if logic bug or disconnected cycles not found?)
  // Actually, isolated nodes have in_degree 0 and are added. 
  // Nodes in a cycle won't be added until cycle is broken. 
  // Since we break all cycles, all alive nodes should be in 'order'.
  // But let's be safe and append any missing ones.
  
  for (size_t i = 0; i < n; ++i) {
    if (alive[i] &&
        std::find(order.begin(), order.end(), static_cast<int>(i)) == order.end()) {
      order.push_back(static_cast<int>(i));
    }
  }

  return {order, aborts, edge_count};
}

bool ValidatorBatcher::FindCycle(const std::vector<std::vector<int>>& edges,
                                 const std::vector<bool>& alive,
                                 std::vector<int>* cycle) const {
  const size_t n = edges.size();
  cycle->clear();
  std::vector<int> state(n, 0); // 0: new, 1: visiting, 2: visited
  std::vector<int> stack;

  std::function<bool(int)> dfs = [&](int node) {
    state[node] = 1;
    stack.push_back(node);
    for (int next : edges[node]) {
      if (!alive[next]) {
        continue;
      }
      if (state[next] == 0) {
        if (dfs(next)) {
          return true;
        }
      } else if (state[next] == 1) {
        // Found cycle
        auto it = std::find(stack.begin(), stack.end(), next);
        cycle->assign(it, stack.end());
        return true;
      }
    }
    stack.pop_back();
    state[node] = 2;
    return false;
  };

  for (size_t i = 0; i < n; ++i) {
    if (alive[i] && state[i] == 0 && dfs(static_cast<int>(i))) {
      return true;
    }
  }
  return false;
}

int ValidatorBatcher::ChooseVictim(const std::vector<std::vector<int>>& edges,
                                   const std::vector<bool>& alive,
                                   const std::vector<int>& cycle) const {
  if (cycle.empty()) {
    return -1;
  }
  int victim = cycle.front();
  size_t best_score = 0;
  
  // Simple heuristic: pick node with most outgoing edges (conflicts with many others)
  // Or most participating in the cycle?
  // Current: node with most total edges
  
  for (int node : cycle) {
    if (!alive[node]) {
      continue;
    }
    size_t score = edges[node].size();
    // Add incoming edges count? For now just outgoing.
    
    switch (policy_.type) {
      case ValidatorPolicyType::kMinAbort:
        if (score > best_score) {
          best_score = score;
          victim = node;
        }
        break;
      case ValidatorPolicyType::kAgePriority:
      case ValidatorPolicyType::kTailLatency:
      case ValidatorPolicyType::kThreadAware:
      default:
        if (score > best_score) {
          best_score = score;
          victim = node;
        }
        break;
    }
  }
  return victim;
}

bool ValidatorBatcher::SharesKey(const PendingValidation& lhs,
                                 const PendingValidation& rhs) const {
  // Deprecated/Not used in new logic, but kept for interface compatibility if needed
  return SharesAny(lhs.write_keys, rhs.read_keys) ||
         SharesAny(lhs.read_keys, rhs.write_keys) ||
         SharesAny(lhs.write_keys, rhs.write_keys);
}

void ValidatorBatcher::ApplyAccessInfo(PendingValidation& pending) {
  if (!tracker_ || pending.req_nr == 0) {
    return;
  }
  AccessInfo info = tracker_->Consume(pending.req_nr);
  pending.read_keys = std::move(info.reads);
  pending.write_keys = std::move(info.writes);
}

}  // namespace mako::txn_reordering
