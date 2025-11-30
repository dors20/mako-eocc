#pragma once

#include <cstdint>
#include <vector>

#include "txn.h"

namespace mako {
namespace occ {

// Simple hash for Row/tuple pointers
using KeyRef = uintptr_t;

template <typename Txn>
struct TxnDescriptor {
  using txn_type = Txn;
  using key_type = KeyRef;
  txn_type* txn{nullptr};
  uint64_t internal_id{0};
  std::vector<KeyRef> read_keys;
  std::vector<KeyRef> write_keys;
  double priority{0.0};
};

// Builds transaction metadata for reordering/graph construction.
template <template <typename> class Protocol, typename Traits>
class TxnBatchBuilder {
 public:
  using txn_type = transaction<Protocol, Traits>;
  using descriptor_type = TxnDescriptor<txn_type>;

  static descriptor_type Build(txn_type* txn) {
    descriptor_type desc;
    desc.txn = txn;
    desc.internal_id = reinterpret_cast<uint64_t>(txn);
    desc.priority = 0.0;

    desc.read_keys.reserve(txn->read_set.size());
    for (auto it = txn->read_set.begin(); it != txn->read_set.end(); ++it) {
      const dbtuple* tuple = it->get_tuple();
      if (tuple != nullptr) {
        desc.read_keys.push_back(reinterpret_cast<KeyRef>(tuple));
      }
    }

    desc.write_keys.reserve(txn->write_set.size());
    for (auto it = txn->write_set.begin(); it != txn->write_set.end(); ++it) {
      dbtuple* tuple = it->get_tuple();
      if (tuple != nullptr) {
        desc.write_keys.push_back(reinterpret_cast<KeyRef>(tuple));
      }
    }
    return desc;
  }

  template <typename InputRange, typename OutputVector>
  static void BuildBatch(const InputRange& txns, OutputVector& out) {
    out.clear();
    out.reserve(txns.size());
    for (auto* txn : txns) {
      if (txn != nullptr) {
        out.emplace_back(Build(txn));
      }
    }
  }
};

}  // namespace occ
}  // namespace mako

