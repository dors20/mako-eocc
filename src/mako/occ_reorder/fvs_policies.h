#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_set>
#include <vector>

#include "mako/occ_reorder/dependency_graph.h"

namespace mako {
namespace occ {

enum class FvsPolicy {
  MIN_ID,
  PROD_DEGREE,
};

struct FvsResult {
  std::unordered_set<uint64_t> remove;
  size_t cyclic_components{0};
  size_t total_components{0};
  size_t total_nodes{0};
};

template <typename Graph>
class FvsSolver {
 public:
  explicit FvsSolver(FvsPolicy policy) : policy_(policy) {}

  FvsResult Compute(const Graph& graph) {
    auto components = graph.strongly_connected_components();
    FvsResult result;
    result.total_components = components.size();
    result.total_nodes = graph.node_count();
    for (const auto& comp : components) {
      if (comp.size() <= 1) {
        continue;
      }
      ++result.cyclic_components;
      uint64_t victim = select_victim(graph, comp);
      result.remove.insert(victim);
    }
    return result;
  }

 private:
  uint64_t select_victim(const Graph& graph, const std::vector<uint64_t>& comp) {
    if (policy_ == FvsPolicy::PROD_DEGREE) {
      return select_by_degree_product(graph, comp);
    }
    return *std::min_element(comp.begin(), comp.end());
  }

  uint64_t select_by_degree_product(const Graph& graph,
                                    const std::vector<uint64_t>& comp) {
    std::unordered_set<uint64_t> scope(comp.begin(), comp.end());
    uint64_t best_id = comp.front();
    size_t best_score = 0;
    double best_priority = std::numeric_limits<double>::lowest();
    for (uint64_t id : comp) {
      size_t idx = graph.index_of(id);
      size_t out_degree = graph.outgoing_count(idx);
      size_t in_degree = graph.incoming_degree(idx, scope);
      size_t score = (in_degree + 1) * (out_degree + 1);
      double priority = graph.priority_of(id);
      if (score > best_score ||
          (score == best_score && priority > best_priority) ||
          (score == best_score && priority == best_priority && id < best_id)) {
        best_score = score;
        best_priority = priority;
        best_id = id;
      }
    }
    return best_id;
  }

  FvsPolicy policy_;
};

}  // namespace occ
}  // namespace mako

