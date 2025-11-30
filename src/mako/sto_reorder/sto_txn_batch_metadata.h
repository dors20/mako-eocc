#pragma once

#include <cstdint>
#include <vector>

#include "benchmarks/sto/Transaction.hh"

namespace mako {
namespace sto {

using StoKeyRef = uintptr_t;

struct StoTxnDescriptor {
  using txn_type = Transaction;
  using key_type = StoKeyRef;

  txn_type* txn{nullptr};
  uint64_t internal_id{0};
  std::vector<key_type> read_keys;
  std::vector<key_type> write_keys;
  double priority{0.0};
};

class StoTxnBatchBuilder {
 public:
  using txn_type = Transaction;
  using descriptor_type = StoTxnDescriptor;

  static descriptor_type Build(txn_type* txn) {
    descriptor_type desc;
    desc.txn = txn;
    desc.internal_id = reinterpret_cast<uint64_t>(txn);
    desc.priority = 0.0;
    txn->for_each_item([&](const TransItem& item) {
      const auto owner = item.owner();
      const auto raw_key = item.raw_key();
      const auto key_ref =
          reinterpret_cast<StoKeyRef>(owner) ^ (reinterpret_cast<StoKeyRef>(raw_key) >> 3);
      if (item.has_read()) {
        desc.read_keys.push_back(key_ref);
      }
      if (item.has_write()) {
        desc.write_keys.push_back(key_ref);
      }
    });
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

}  // namespace sto
}  // namespace mako


