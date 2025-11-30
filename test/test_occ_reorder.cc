#include <algorithm>
#include <cstdint>
#include <iostream>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include "mako/occ_reorder/dependency_graph.h"
#include "mako/occ_reorder/fvs_policies.h"

namespace {

struct TestDescriptor {
  using key_type = uintptr_t;
  uint64_t internal_id{0};
  double priority{0.0};
  std::vector<key_type> read_keys;
  std::vector<key_type> write_keys;
};

using Descriptor = TestDescriptor;
using Builder = mako::occ::DependencyGraphBuilder<Descriptor>;

Descriptor MakeTxn(uint64_t id,
                   std::initializer_list<uintptr_t> reads,
                   std::initializer_list<uintptr_t> writes) {
  Descriptor desc;
  desc.internal_id = id;
  desc.priority = static_cast<double>(id);
  desc.read_keys.assign(reads.begin(), reads.end());
  desc.write_keys.assign(writes.begin(), writes.end());
  return desc;
}

}  // namespace

TEST(DependencyGraphTest, BuildsReadWriteEdges) {
  std::vector<Descriptor> txns;
  txns.push_back(MakeTxn(1, {}, {1}));
  txns.push_back(MakeTxn(2, {1}, {}));
  txns.push_back(MakeTxn(3, {1}, {2}));

  Builder builder;
  auto graph = builder.build(txns);
  EXPECT_EQ(graph.node_count(), 3u);
  EXPECT_EQ(graph.edge_count(), 2u);  // readers depend on writer

  auto order = graph.topo_sort_without({});
  ASSERT_EQ(order.size(), 3u);
  // Txn 1 must precede {2,3}, and txn 3 precede none.
  auto pos = [&](uint64_t id) {
    auto it = std::find(order.begin(), order.end(), id);
    EXPECT_NE(it, order.end());
    return static_cast<size_t>(std::distance(order.begin(), it));
  };
  size_t pos1 = pos(1);
  size_t pos2 = pos(2);
  size_t pos3 = pos(3);
  EXPECT_GT(pos1, pos2);
  EXPECT_GT(pos1, pos3);
  std::cout << "[Reorder] topo order after baseline edges: "
            << order[0] << " -> " << order[1] << " -> " << order[2] << std::endl;
}

TEST(DependencyGraphTest, DetectsStrongComponents) {
  std::vector<Descriptor> txns;
  txns.push_back(MakeTxn(10, {}, {1}));
  txns.push_back(MakeTxn(11, {1}, {2}));
  txns.push_back(MakeTxn(12, {2}, {1}));

  Builder builder;
  auto graph = builder.build(txns);
  auto components = graph.strongly_connected_components();
  bool cycle_found = false;
  for (const auto& comp : components) {
    if (comp.size() == 3) {
      cycle_found = true;
      break;
    }
  }
  EXPECT_TRUE(cycle_found);

  std::unordered_set<uint64_t> removed = {11};
  auto trimmed = graph.strongly_connected_components(removed);
  for (const auto& comp : trimmed) {
    EXPECT_LE(comp.size(), 1u);
  }
}

TEST(DependencyGraphTest, ReordersAfterRemovingCycleNode) {
  std::vector<Descriptor> txns;
  txns.push_back(MakeTxn(21, {}, {5}));
  txns.push_back(MakeTxn(22, {5}, {6}));
  txns.push_back(MakeTxn(23, {6}, {5}));
  txns.push_back(MakeTxn(24, {6}, {}));

  Builder builder;
  auto graph = builder.build(txns);
  auto scc = graph.strongly_connected_components();
  bool found_cycle = false;
  for (const auto& comp : scc) {
    if (comp.size() > 1) {
      found_cycle = true;
      break;
    }
  }
  EXPECT_TRUE(found_cycle);

  std::unordered_set<uint64_t> remove = {22};
  auto order = graph.topo_sort_without(remove);
  ASSERT_EQ(order.size(), 3u);
  auto pos = [&](uint64_t id) {
    auto it = std::find(order.begin(), order.end(), id);
    EXPECT_NE(it, order.end());
    return static_cast<size_t>(std::distance(order.begin(), it));
  };
  size_t pos21 = pos(21);
  size_t pos23 = pos(23);
  size_t pos24 = pos(24);
  EXPECT_LT(pos21, pos24);
  EXPECT_LT(pos24, pos23);

  std::cout << "[Reorder] removed txn 22, topo order: ";
  for (size_t i = 0; i < order.size(); ++i) {
    std::cout << order[i] << (i + 1 < order.size() ? " -> " : "");
  }
  std::cout << std::endl;
}

TEST(FvsSolverTest, DegreePolicyTargetsHighDegreeNodes) {
  mako::occ::DependencyGraph graph;
  graph.add_node(101, 0.0);
  graph.add_node(102, 0.0);
  graph.add_node(103, 0.0);

  graph.add_edge(101, 102);
  graph.add_edge(102, 101);
  graph.add_edge(102, 103);

  mako::occ::FvsSolver<mako::occ::DependencyGraph> solver(
      mako::occ::FvsPolicy::PROD_DEGREE);
  auto result = solver.Compute(graph);
  ASSERT_EQ(result.remove.size(), 1u);
  EXPECT_TRUE(result.remove.count(102));
  EXPECT_EQ(result.cyclic_components, 1u);
}

