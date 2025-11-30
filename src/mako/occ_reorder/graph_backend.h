#pragma once

#include <unordered_set>
#include <vector>

#include "occ_reorder/dependency_graph.h"
#include "occ_reorder/fvs_policies.h"

namespace mako {
namespace occ {

struct GraphPlan {
  std::vector<uint64_t> ordered_ids;
  std::unordered_set<uint64_t> abort_ids;
  size_t graph_nodes{0};
  size_t graph_edges{0};
  size_t removed_nodes{0};
  size_t cycle_components{0};
  size_t total_components{0};
};

template <typename Descriptor, typename Options>
class SerialGraphBackend {
 public:
  using descriptor_type = Descriptor;

  static GraphPlan Compute(const std::vector<descriptor_type>& descriptors,
                           const Options& options) {
    GraphPlan plan;
    if (descriptors.size() < 2) {
      plan.graph_nodes = descriptors.size();
      plan.graph_edges = 0;
      plan.ordered_ids.reserve(descriptors.size());
      for (const auto& desc : descriptors) {
        plan.ordered_ids.push_back(desc.internal_id);
      }
      return plan;
    }

    DependencyGraphBuilder<descriptor_type> builder;
    auto graph = builder.build(descriptors);
    plan.graph_nodes = graph.node_count();
    plan.graph_edges = graph.edge_count();

    FvsSolver<DependencyGraph> solver(options.fvs_policy);
    auto fvs = solver.Compute(graph);
    plan.abort_ids = std::move(fvs.remove);
    plan.removed_nodes = plan.abort_ids.size();
    plan.cycle_components = fvs.cyclic_components;
    plan.total_components = fvs.total_components;

    if (options.require_cycle && plan.abort_ids.empty()) {
      plan.ordered_ids = graph.topo_sort_without({});
      return plan;
    }

    plan.ordered_ids = graph.topo_sort_without(plan.abort_ids);
    return plan;
  }
};

}  // namespace occ
}  // namespace mako


