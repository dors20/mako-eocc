#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <stack>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mako {
namespace occ {

class DependencyGraph {
 public:
  size_t add_node(uint64_t txn_id, double priority) {
    auto [it, inserted] = id_to_index_.emplace(txn_id, nodes_.size());
    if (inserted) {
      nodes_.push_back(Node{txn_id, priority, {}});
      return nodes_.size() - 1;
    }
    return it->second;
  }

  void add_edge(uint64_t from, uint64_t to) {
    if (from == to) {
      return;
    }
    auto it_from = id_to_index_.find(from);
    auto it_to = id_to_index_.find(to);
    if (it_from == id_to_index_.end() || it_to == id_to_index_.end()) {
      return;
    }
    nodes_[it_from->second].outgoing.push_back(it_to->second);
    ++edge_count_;
  }

  size_t node_count() const { return nodes_.size(); }
  size_t edge_count() const { return edge_count_; }

  size_t index_of(uint64_t id) const {
    auto it = id_to_index_.find(id);
    return (it == id_to_index_.end()) ? nodes_.size() : it->second;
  }

  uint64_t id_from_index(size_t idx) const { return nodes_.at(idx).id; }

  double priority_of(uint64_t id) const {
    auto it = id_to_index_.find(id);
    if (it == id_to_index_.end()) {
      return 0.0;
    }
    return nodes_[it->second].priority;
  }

  size_t outgoing_count(size_t idx) const {
    if (idx >= nodes_.size()) {
      return 0;
    }
    return nodes_[idx].outgoing.size();
  }

  size_t incoming_degree(size_t idx,
                         const std::unordered_set<uint64_t>& scope) const {
    if (idx >= nodes_.size()) {
      return 0;
    }
    size_t count = 0;
    for (uint64_t id : scope) {
      auto it = id_to_index_.find(id);
      if (it == id_to_index_.end()) {
        continue;
      }
      size_t v = it->second;
      for (size_t neighbor : nodes_[v].outgoing) {
        if (neighbor == idx) {
          ++count;
        }
      }
    }
    return count;
  }

  const std::vector<size_t>& outgoing_indices(size_t idx) const {
    static const std::vector<size_t> kEmpty;
    if (idx >= nodes_.size()) {
      return kEmpty;
    }
    return nodes_[idx].outgoing;
  }

  std::vector<uint64_t> topo_sort_without(
      const std::unordered_set<uint64_t>& remove) const {
    std::vector<int> indegree(nodes_.size(), 0);
    std::unordered_set<size_t> removed_indices;
    removed_indices.reserve(remove.size());
    for (uint64_t id : remove) {
      auto it = id_to_index_.find(id);
      if (it != id_to_index_.end()) {
        removed_indices.insert(it->second);
      }
    }
    for (size_t idx = 0; idx < nodes_.size(); ++idx) {
      if (removed_indices.count(idx)) {
        continue;
      }
      for (size_t neighbor : nodes_[idx].outgoing) {
        if (removed_indices.count(neighbor)) {
          continue;
        }
        indegree[neighbor]++;
      }
    }
    std::vector<uint64_t> order;
    order.reserve(nodes_.size() - removed_indices.size());
    std::vector<size_t> queue;
    queue.reserve(nodes_.size());
    for (size_t idx = 0; idx < nodes_.size(); ++idx) {
      if (!removed_indices.count(idx) && indegree[idx] == 0) {
        queue.push_back(idx);
      }
    }
    for (size_t head = 0; head < queue.size(); ++head) {
      size_t idx = queue[head];
      order.push_back(nodes_[idx].id);
      for (size_t neighbor : nodes_[idx].outgoing) {
        if (removed_indices.count(neighbor)) {
          continue;
        }
        indegree[neighbor]--;
        if (indegree[neighbor] == 0) {
          queue.push_back(neighbor);
        }
      }
    }
    return order;
  }

  std::vector<std::vector<uint64_t>> strongly_connected_components() const {
    return strongly_connected_components({});
  }

  std::vector<std::vector<uint64_t>> strongly_connected_components(
      const std::unordered_set<uint64_t>& removed) const {
    std::vector<int> index(nodes_.size(), -1);
    std::vector<int> lowlink(nodes_.size(), -1);
    std::vector<bool> on_stack(nodes_.size(), false);
    std::stack<size_t> stack;
    std::vector<std::vector<uint64_t>> components;
    int current_index = 0;

    std::unordered_set<size_t> removed_indices;
    removed_indices.reserve(removed.size());
    for (uint64_t id : removed) {
      auto it = id_to_index_.find(id);
      if (it != id_to_index_.end()) {
        removed_indices.insert(it->second);
      }
    }

    std::function<void(size_t)> strong_connect = [&](size_t v) {
      if (removed_indices.count(v)) {
        return;
      }
      index[v] = current_index;
      lowlink[v] = current_index;
      ++current_index;
      stack.push(v);
      on_stack[v] = true;

      for (size_t w : nodes_[v].outgoing) {
        if (removed_indices.count(w)) {
          continue;
        }
        if (index[w] == -1) {
          strong_connect(w);
          lowlink[v] = std::min(lowlink[v], lowlink[w]);
        } else if (on_stack[w]) {
          lowlink[v] = std::min(lowlink[v], index[w]);
        }
      }

      if (lowlink[v] == index[v]) {
        std::vector<uint64_t> component;
        size_t w = 0;
        do {
          w = stack.top();
          stack.pop();
          on_stack[w] = false;
          component.push_back(nodes_[w].id);
        } while (w != v);
        components.emplace_back(std::move(component));
      }
    };

    for (size_t v = 0; v < nodes_.size(); ++v) {
      if (index[v] == -1 && !removed_indices.count(v)) {
        strong_connect(v);
      }
    }
    return components;
  }

  template <typename Fn>
  void for_each_node(const Fn& fn) const {
    for (const auto& node : nodes_) {
      fn(node.id, node.outgoing);
    }
  }

 private:
  struct Node {
    uint64_t id;
    double priority;
    std::vector<size_t> outgoing;
  };

  std::vector<Node> nodes_;
  std::unordered_map<uint64_t, size_t> id_to_index_;
  size_t edge_count_{0};
};

template <typename TxnDescriptor>
class DependencyGraphBuilder {
 public:
  DependencyGraph build(const std::vector<TxnDescriptor>& descriptors) const {
    DependencyGraph graph;
    for (const auto& desc : descriptors) {
      graph.add_node(desc.internal_id, desc.priority);
    }

    using KeyRef = typename TxnDescriptor::key_type;
    std::unordered_map<KeyRef, std::vector<uint64_t>> write_map;
    for (const auto& desc : descriptors) {
      for (KeyRef key : desc.write_keys) {
        write_map[key].push_back(desc.internal_id);
      }
    }
    for (auto& entry : write_map) {
      auto& writers = entry.second;
      std::sort(writers.begin(), writers.end());
      for (size_t i = 1; i < writers.size(); ++i) {
        graph.add_edge(writers[i - 1], writers[i]);
      }
    }

    for (const auto& desc : descriptors) {
      for (KeyRef key : desc.read_keys) {
        auto it = write_map.find(key);
        if (it == write_map.end()) {
          continue;
        }
        for (uint64_t writer : it->second) {
          if (writer != desc.internal_id) {
            graph.add_edge(desc.internal_id, writer);
          }
        }
      }
    }
    return graph;
  }
};

}  // namespace occ
}  // namespace mako

