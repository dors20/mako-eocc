#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_set>
#include <vector>

#include "mako/occ_reorder/dependency_graph.h"

namespace mako {
namespace occ {

// Policy for ranking vertices during FVS selection.
enum class FvsPolicy {
  MIN_ID,
  PROD_DEGREE,
};

// Algorithm family for FVS computation (Ding et al. inspired).
enum class FvsAlgorithm {
  BASIC_SCC,    // One victim per SCC (legacy / very cheap)
  SORT_GREEDY,  // Sort-based greedy with multi-factor k
  HYBRID,       // Reserved for future hybrid implementation
};

struct FvsConfig {
  FvsPolicy policy{FvsPolicy::MIN_ID};
  FvsAlgorithm algorithm{FvsAlgorithm::BASIC_SCC};
  size_t sort_k{2};             // multi-factor k in sort-based greedy
  size_t hybrid_threshold{5};   // unused for now, reserved
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
  // Backwards-compatible ctor: keeps legacy BASIC_SCC behaviour.
  explicit FvsSolver(FvsPolicy policy) {
    config_.policy = policy;
    config_.algorithm = FvsAlgorithm::BASIC_SCC;
    config_.sort_k = 2;
    config_.hybrid_threshold = 5;
  }

  explicit FvsSolver(const FvsConfig& cfg) : config_(cfg) {}

  FvsResult Compute(const Graph& graph) {
    FvsResult result;
    result.total_nodes = graph.node_count();

    // Collect SCC statistics once on the original graph.
    auto components = graph.strongly_connected_components();
    result.total_components = components.size();
    for (const auto& comp : components) {
      if (comp.size() > 1) {
        ++result.cyclic_components;
      }
    }

    if (result.total_nodes < 2 || result.cyclic_components == 0) {
      // No cycles, nothing to remove.
      return result;
    }

    switch (config_.algorithm) {
      case FvsAlgorithm::SORT_GREEDY:
        result.remove = ComputeSortGreedy(graph);
        break;
      case FvsAlgorithm::HYBRID:
        // For now, fall back to sort-based greedy; hybrid can be added later.
        result.remove = ComputeSortGreedy(graph);
        break;
      case FvsAlgorithm::BASIC_SCC:
      default:
        // Legacy very-cheap heuristic: one victim per SCC.
        for (const auto& comp : components) {
          if (comp.size() <= 1) {
            continue;
          }
          uint64_t victim = select_victim(graph, comp);
          result.remove.insert(victim);
        }
        break;
    }
    return result;
  }

 private:
  struct NodeScore {
    uint64_t id{0};
    size_t in_degree{0};
    size_t out_degree{0};
    size_t score{0};
    double priority{0.0};
  };

  uint64_t select_victim(const Graph& graph,
                         const std::vector<uint64_t>& comp) const {
    if (config_.policy == FvsPolicy::PROD_DEGREE) {
      return select_by_degree_product(graph, comp);
    }
    // MIN_ID policy.
    return *std::min_element(comp.begin(), comp.end());
  }

  uint64_t select_by_degree_product(const Graph& graph,
                                    const std::vector<uint64_t>& comp) const {
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

  // Compute a FVS using the sort-based greedy algorithm from Ding et al.
  std::unordered_set<uint64_t> ComputeSortGreedy(const Graph& graph) const {
    std::unordered_set<uint64_t> fvs;
    const size_t n = graph.node_count();
    if (n < 2) {
      return fvs;
    }

    // Active set of node ids we are still considering in the trimmed graph.
    std::unordered_set<uint64_t> active_ids;
    active_ids.reserve(n);
    graph.for_each_node(
        [&](uint64_t id, const std::vector<size_t>& /*outgoing*/) {
          active_ids.insert(id);
        });

    if (active_ids.size() < 2) {
      return fvs;
    }

    const size_t base_k =
        config_.sort_k == 0 ? static_cast<size_t>(2) : config_.sort_k;

    // Helper buffers reused across iterations.
    const size_t node_count = graph.node_count();
    std::vector<size_t> indegree(node_count);
    std::vector<size_t> outdegree(node_count);
    std::vector<char> is_active(node_count);

    auto trim_and_score = [&](std::vector<NodeScore>& scores) {
      std::fill(indegree.begin(), indegree.end(), 0);
      std::fill(outdegree.begin(), outdegree.end(), 0);
      std::fill(is_active.begin(), is_active.end(), 0);

      // Mark active indices.
      for (uint64_t id : active_ids) {
        size_t idx = graph.index_of(id);
        if (idx < node_count) {
          is_active[idx] = 1;
        }
      }

      // Compute in/out degrees restricted to active subgraph.
      graph.for_each_node([&](uint64_t id, const std::vector<size_t>& outs) {
        size_t u = graph.index_of(id);
        if (u >= node_count || !is_active[u]) {
          return;
        }
        for (size_t v : outs) {
          if (v >= node_count || !is_active[v]) {
            continue;
          }
          ++outdegree[u];
          ++indegree[v];
        }
      });

      // Trim vertices that cannot be part of any cycle (indegree==0 or
      // outdegree==0). They are not added to the FVS.
      std::vector<uint64_t> to_trim;
      to_trim.reserve(active_ids.size());
      for (uint64_t id : active_ids) {
        size_t idx = graph.index_of(id);
        if (idx >= node_count) {
          continue;
        }
        if (indegree[idx] == 0 || outdegree[idx] == 0) {
          to_trim.push_back(id);
        }
      }
      for (uint64_t id : to_trim) {
        active_ids.erase(id);
      }

      scores.clear();
      scores.reserve(active_ids.size());
      for (uint64_t id : active_ids) {
        size_t idx = graph.index_of(id);
        if (idx >= node_count) {
          continue;
        }
        NodeScore ns;
        ns.id = id;
        ns.in_degree = indegree[idx];
        ns.out_degree = outdegree[idx];
        ns.score = (config_.policy == FvsPolicy::PROD_DEGREE)
                       ? (ns.in_degree + 1) * (ns.out_degree + 1)
                       : 1;
        ns.priority = graph.priority_of(id);
        scores.push_back(ns);
      }
    };

    std::vector<NodeScore> scores;
    trim_and_score(scores);

    while (!active_ids.empty() && !scores.empty()) {
      size_t remaining = active_ids.size();
      size_t k = base_k;
      if (remaining <= k) {
        // When few vertices remain, fall back to k=1 to avoid over-removal.
        k = 1;
      }
      if (k == 0) {
        k = 1;
      }

      // Sort scores by descending (score, priority) and ascending id.
      auto better = [](const NodeScore& a, const NodeScore& b) {
        if (a.score != b.score) {
          return a.score > b.score;
        }
        if (a.priority != b.priority) {
          return a.priority > b.priority;
        }
        return a.id < b.id;
      };

      if (scores.size() <= k) {
        std::sort(scores.begin(), scores.end(), better);
      } else {
        std::partial_sort(scores.begin(),
                          scores.begin() +
                              static_cast<std::ptrdiff_t>(k),
                          scores.end(), better);
      }

      // Select up to k highest-ranked vertices into the FVS.
      size_t selected = std::min(k, scores.size());
      for (size_t i = 0; i < selected; ++i) {
        uint64_t id = scores[i].id;
        if (active_ids.erase(id) > 0) {
          fvs.insert(id);
        }
      }

      // Recompute degrees and trim again on the reduced graph.
      trim_and_score(scores);
    }

    return fvs;
  }

  FvsConfig config_;
};

}  // namespace occ
}  // namespace mako

