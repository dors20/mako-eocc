#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "occ_reorder/graph_backend.h"
#include "occ_reorder/fvs_policies.h"
template <template <typename> class Protocol, typename Traits>
class transaction;

namespace mako {
namespace occ {

template <template <typename> class Protocol, typename Traits>
class TxnBatchBuilder;

template <typename Txn,
          typename DescriptorBuilder,
          template <typename, typename> class GraphBackend = SerialGraphBackend>
class GenericTxnReorderController {
 public:
  struct Options;
  using txn_type = Txn;
  using descriptor_type = typename DescriptorBuilder::descriptor_type;
  using backend_type = GraphBackend<descriptor_type, Options>;

  struct Result {
    bool applied{false};
    std::vector<txn_type*> ordered;
    std::vector<txn_type*> aborted;
    size_t graph_nodes{0};
    size_t graph_edges{0};
    size_t removed_nodes{0};
    size_t cycle_components{0};
    size_t total_components{0};
  };

  struct Options {
    FvsPolicy fvs_policy{FvsPolicy::MIN_ID};
    // Default to sort-based greedy as it offers the best trade-off in Ding et al.
    FvsAlgorithm fvs_algorithm{FvsAlgorithm::SORT_GREEDY};
    size_t fvs_sort_k{2};
    size_t fvs_hybrid_threshold{5};
    bool require_cycle{true};
  };

  static Result Plan(const std::vector<txn_type*>& txns,
                     const Options& options = DefaultOptions()) {
    Result result;
    if (txns.size() < 2) {
      return result;
    }

    std::vector<descriptor_type> descriptors;
    DescriptorBuilder::BuildBatch(txns, descriptors);
    auto plan = backend_type::Compute(descriptors, options);
    result.graph_nodes = plan.graph_nodes;
    result.graph_edges = plan.graph_edges;
    result.removed_nodes = plan.removed_nodes;
    result.cycle_components = plan.cycle_components;
    result.total_components = plan.total_components;

    if (options.require_cycle && plan.abort_ids.empty()) {
      return result;
    }

    std::unordered_map<uint64_t, txn_type*> id_map;
    id_map.reserve(descriptors.size());
    for (const auto& desc : descriptors) {
      id_map.emplace(desc.internal_id, desc.txn);
      if (plan.abort_ids.count(desc.internal_id)) {
        result.aborted.push_back(desc.txn);
      }
    }
    for (uint64_t id : plan.ordered_ids) {
      auto it = id_map.find(id);
      if (it != id_map.end()) {
        result.ordered.push_back(it->second);
      }
    }

    if (result.ordered.empty()) {
      return result;
    }

    std::vector<uint64_t> original_ids;
    original_ids.reserve(txns.size());
    for (auto* txn : txns) {
      uint64_t id = reinterpret_cast<uint64_t>(txn);
      if (!plan.abort_ids.count(id)) {
        original_ids.push_back(id);
      }
    }

    bool same_order =
        (original_ids.size() == result.ordered.size()) &&
        std::equal(original_ids.begin(),
                   original_ids.end(),
                   result.ordered.begin(),
                   [](uint64_t id, txn_type* txn) {
                     return id == reinterpret_cast<uint64_t>(txn);
                   });
    result.applied = !same_order || !plan.abort_ids.empty();
    if (!result.applied) {
      result.ordered.clear();
      result.aborted.clear();
    }
    return result;
  }

 private:
  static Options LoadOptionsFromEnv() {
    Options opts;
    if (const char* env = std::getenv("MAKO_TXN_REORDER_FVS")) {
      std::string policy(env);
      for (auto& c : policy) {
        c = static_cast<char>(std::tolower(c));
      }
      if (policy == "prod" || policy == "degree" || policy == "prod_degree") {
        opts.fvs_policy = FvsPolicy::PROD_DEGREE;
      } else {
        opts.fvs_policy = FvsPolicy::MIN_ID;
      }
    }
    if (const char* env = std::getenv("MAKO_TXN_REORDER_ALGO")) {
      std::string algo(env);
      for (auto& c : algo) {
        c = static_cast<char>(std::tolower(c));
      }
      if (algo == "sort" || algo == "sort_greedy") {
        opts.fvs_algorithm = FvsAlgorithm::SORT_GREEDY;
      } else if (algo == "hybrid") {
        opts.fvs_algorithm = FvsAlgorithm::HYBRID;
      } else {
        opts.fvs_algorithm = FvsAlgorithm::BASIC_SCC;
      }
    }
    if (const char* env = std::getenv("MAKO_TXN_REORDER_SORT_K")) {
      char* end = nullptr;
      unsigned long v = std::strtoul(env, &end, 10);
      if (end != env && v > 0) {
        opts.fvs_sort_k = static_cast<size_t>(v);
      }
    }
    if (const char* env = std::getenv("MAKO_TXN_REORDER_HYBRID_THRESHOLD")) {
      char* end = nullptr;
      unsigned long v = std::strtoul(env, &end, 10);
      if (end != env && v > 0) {
        opts.fvs_hybrid_threshold = static_cast<size_t>(v);
      }
    }
    if (const char* env = std::getenv("MAKO_TXN_REORDER_REQUIRE_CYCLE")) {
      std::string flag(env);
      opts.require_cycle =
          !(flag == "0" || flag == "false" || flag == "FALSE");
    }
    return opts;
  }

  static const Options& DefaultOptions() {
    static Options opts = LoadOptionsFromEnv();
    return opts;
  }
};

template <template <typename> class Protocol, typename Traits>
using TxnReorderController =
    GenericTxnReorderController<transaction<Protocol, Traits>, TxnBatchBuilder<Protocol, Traits>>;

}  // namespace occ
}  // namespace mako

