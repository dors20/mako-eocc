#pragma once

#include <algorithm>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <cstdlib>

#include "occ_reorder/graph_backend.h"
#include "occ_reorder/dependency_graph.h"
#include "occ_reorder/fvs_policies.h"

namespace mako {
namespace occ {

template <typename Descriptor, typename Options>
class ParallelGraphBackend {
 public:
  using descriptor_type = Descriptor;

  static GraphPlan Compute(const std::vector<descriptor_type>& descriptors,
                           const Options& options) {
    constexpr size_t kParallelThreshold = 32;
    if (descriptors.size() < kParallelThreshold) {
      return SerialGraphBackend<descriptor_type, Options>::Compute(descriptors, options);
    }

    size_t concurrency = 0;
    if (const char* env = std::getenv("MAKO_TXN_REORDER_THREADS")) {
      char* end = nullptr;
      unsigned long v = std::strtoul(env, &end, 10);
      if (end != env && v > 0) {
        concurrency = static_cast<size_t>(v);
      }
    }
    if (concurrency == 0) {
      concurrency = std::thread::hardware_concurrency();
      if (concurrency == 0) {
        concurrency = 4;
      }
    }
    // Clamp to a reasonable range: at least 2 threads, at most one per descriptor.
    concurrency = std::max<size_t>(2, std::min(concurrency, descriptors.size()));

    DependencyGraph graph;
    graph = build_graph(descriptors, concurrency);

    GraphPlan plan;
    plan.graph_nodes = graph.node_count();
    plan.graph_edges = graph.edge_count();

    FvsConfig fcfg;
    fcfg.policy = options.fvs_policy;
    fcfg.algorithm = options.fvs_algorithm;
    fcfg.sort_k = options.fvs_sort_k;
    fcfg.hybrid_threshold = options.fvs_hybrid_threshold;
    FvsSolver<DependencyGraph> solver(fcfg);
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

 private:
  using KeyRef = typename descriptor_type::key_type;
  using Edge = std::pair<uint64_t, uint64_t>;

  static DependencyGraph build_graph(const std::vector<descriptor_type>& descriptors,
                                     size_t concurrency) {
    DependencyGraph graph;
    for (const auto& desc : descriptors) {
      graph.add_node(desc.internal_id, desc.priority);
    }

    std::vector<std::unordered_map<KeyRef, std::vector<uint64_t>>> local_write_maps(concurrency);
    std::vector<std::thread> threads;
    threads.reserve(concurrency);

    auto chunk = [&](size_t idx) {
      size_t begin = (descriptors.size() * idx) / concurrency;
      size_t end = (descriptors.size() * (idx + 1)) / concurrency;
      auto& local_map = local_write_maps[idx];
      for (size_t i = begin; i < end; ++i) {
        const auto& desc = descriptors[i];
        for (KeyRef key : desc.write_keys) {
          local_map[key].push_back(desc.internal_id);
        }
      }
    };

    for (size_t idx = 0; idx < concurrency; ++idx) {
      threads.emplace_back(chunk, idx);
    }
    for (auto& t : threads) {
      t.join();
    }

    std::unordered_map<KeyRef, std::vector<uint64_t>> write_map;
    write_map.reserve(descriptors.size() * 2);
    for (auto& local_map : local_write_maps) {
      for (auto& kv : local_map) {
        auto& dest = write_map[kv.first];
        dest.insert(dest.end(),
                    std::make_move_iterator(kv.second.begin()),
                    std::make_move_iterator(kv.second.end()));
      }
    }

    std::vector<Edge> edges;
    edges.reserve(descriptors.size() * 4);

    for (auto& kv : write_map) {
      auto& writers = kv.second;
      if (writers.size() < 2) {
        continue;
      }
      std::sort(writers.begin(), writers.end());
      for (size_t i = 1; i < writers.size(); ++i) {
        edges.emplace_back(writers[i - 1], writers[i]);
      }
    }

    // Parallelize read edges generation
    std::vector<std::vector<Edge>> read_edges(concurrency);
    threads.clear();
    auto read_chunk = [&](size_t idx) {
      size_t begin = (descriptors.size() * idx) / concurrency;
      size_t end = (descriptors.size() * (idx + 1)) / concurrency;
      auto& local_edges = read_edges[idx];
      local_edges.reserve((end - begin) * 4);
      for (size_t i = begin; i < end; ++i) {
        const auto& desc = descriptors[i];
        for (KeyRef key : desc.read_keys) {
          auto it = write_map.find(key);
          if (it == write_map.end()) {
            continue;
          }
          for (uint64_t writer : it->second) {
            if (writer != desc.internal_id) {
              local_edges.emplace_back(desc.internal_id, writer);
            }
          }
        }
      }
    };

    for (size_t idx = 0; idx < concurrency; ++idx) {
      threads.emplace_back(read_chunk, idx);
    }
    for (auto& t : threads) {
      t.join();
    }

    for (auto& local_edges : read_edges) {
      edges.insert(edges.end(),
                   std::make_move_iterator(local_edges.begin()),
                   std::make_move_iterator(local_edges.end()));
    }

    for (const auto& [from, to] : edges) {
      graph.add_edge(from, to);
    }

    return graph;
  }
};

}  // namespace occ
}  // namespace mako


