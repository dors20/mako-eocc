#pragma once

#include "abstract_db.h"
#include "../txn_proto2_impl.h"
#include "ndb_wrapper.h"
#include "ndb_wrapper_impl.h"

#include <map>
#include <tuple>
#include <vector>

/**
 * OCC-based DB implementation for Mako benchmarks.
 *
 * This wraps the existing OCC engine (`transaction_proto2` + Masstree)
 * behind the `abstract_db` interface so that TPCC/dbtest can run without
 * going through the STO engine.
 *
 * All higher-level OCC optimizations (batch validation, Ding-style FVS
 * reordering, parallel validation) are enabled/disabled via the existing
 * MAKO_* environment variables and are implemented inside the OCC
 * transaction path (see txn_impl.h and txn_occ_batch_validation.h).
 *
 * When MAKO_TXN_ENGINE=occ is set, `initWithDB()` will instantiate this
 * database instead of the STO-backed `mbta_wrapper`.
 */
class occ_db : public abstract_db {
public:
  occ_db()
      : occ_impl_({}, {}, /*call_fsync=*/false, /*use_compression=*/false,
                  /*fake_writes=*/false) {
    // Reserve table id 0 as unused, so real ids start at 1.
    tables_by_id_.push_back(nullptr);
  }

  ~occ_db() override = default;

  // ---- Transaction management ----

  ssize_t txn_max_batch_size() const override {
    return occ_impl_.txn_max_batch_size();
  }

  void do_txn_epoch_sync() const override {
    occ_impl_.do_txn_epoch_sync();
  }

  void do_txn_finish() const override {
    occ_impl_.do_txn_finish();
  }

  size_t sizeof_txn_object(uint64_t txn_flags) const override {
    return occ_impl_.sizeof_txn_object(txn_flags);
  }

  void thread_init(bool loader, int /*source*/ = 0) override {
    occ_impl_.thread_init(loader);
  }

  void thread_end() override {
    occ_impl_.thread_end();
  }

  std::tuple<uint64_t, uint64_t, double>
  get_ntxn_persisted() const override {
    return occ_impl_.get_ntxn_persisted();
  }

  void reset_ntxn_persisted() override {
    occ_impl_.reset_ntxn_persisted();
  }

  void *new_txn(uint64_t txn_flags,
                str_arena &arena,
                void *buf,
                TxnProfileHint hint = HINT_DEFAULT) override {
    return occ_impl_.new_txn(txn_flags, arena, buf, hint);
  }

  counter_map get_txn_counters(void *txn) const override {
    return occ_impl_.get_txn_counters(txn);
  }

  bool commit_txn(void *txn) override {
    return occ_impl_.commit_txn(txn);
  }

  bool commit_txn_no_paxos(void *txn) override {
    // OCC path has no Paxos integration; just commit normally.
    return occ_impl_.commit_txn(txn);
  }

  void abort_txn(void *txn) override {
    occ_impl_.abort_txn(txn);
  }

  void abort_txn_local(void *txn) override {
    occ_impl_.abort_txn_local(txn);
  }

  void print_txn_debug(void *txn) const override {
    occ_impl_.print_txn_debug(txn);
  }

  // ---- Index management ----

  abstract_ordered_index *
  open_index(const std::string &name,
             size_t value_size_hint,
             bool mostly_append = false,
             bool use_hashtable = false) override {
    abstract_ordered_index *idx =
        occ_impl_.open_index(name, value_size_hint, mostly_append,
                             use_hashtable);
    register_table(name, /*shard_index=*/0, idx);
    return idx;
  }

  void close_index(abstract_ordered_index *idx) override {
    occ_impl_.close_index(idx);
  }

  void preallocate_open_index() override {
    // OCC path lazily allocates indexes on demand.
  }

  abstract_ordered_index *
  open_index(const std::string &name, int shard_index = -1) override {
    const int effective_shard = shard_index < 0 ? 0 : shard_index;
    const auto key = std::make_tuple(name, effective_shard);
    auto it = tables_taken_.find(key);
    if (it != tables_taken_.end()) {
      const unsigned short table_id = it->second;
      if (table_id < tables_by_id_.size()) {
        return tables_by_id_[table_id];
      }
    }
    // For OCC we ignore value_size_hint/mostly_append hints here;
    // TPCC code primarily relies on key layout, not these hints.
    abstract_ordered_index *idx =
        occ_impl_.open_index(name, /*value_size_hint=*/0,
                             /*mostly_append=*/false,
                             /*use_hashtable=*/false);
    register_table(name, effective_shard, idx);
    return idx;
  }

  abstract_ordered_index *
  get_index_by_table_id(unsigned short table_id) override {
    if (table_id == 0 || table_id >= tables_by_id_.size()) {
      return nullptr;
    }
    return tables_by_id_[table_id];
  }

  mbta_sharded_ordered_index *
  open_sharded_index(const std::string & /*name*/) override {
    // OCC engine does not support mbta_sharded_ordered_index;
    // not needed for current OCC experiments.
    NDB_UNIMPLEMENTED("open_sharded_index not supported in occ_db");
  }

  // ---- Shard / replication hooks (no-op for single-node OCC path) ----

  void shard_abort_txn(void *txn) override {
    abort_txn(txn);
  }

  int shard_validate() override {
    // No separate shard validation step in this OCC path.
    return 0;
  }

  void shard_install(uint32_t /*timestamp*/) override {
    // No-op: OCC path has no Paxos install stage.
  }

  void shard_serialize_util(uint32_t /*timestamp*/) override {
    // No-op in this configuration.
  }

  void shard_unlock(bool /*committed*/) override {
    // No-op: OCC engine manages its own locks/epochs internally.
  }

  void shard_reset() override {
    // No-op.
  }

  // ---- Initialization ----

  void init() override {
    // Nothing special to do here; indexes are opened lazily.
  }

private:
  using Proto = transaction_proto2;
  ndb_wrapper<Proto> occ_impl_;

  // Simple table registry to satisfy get_index_by_table_id().
  std::vector<abstract_ordered_index *> tables_by_id_;
  std::map<std::tuple<std::string, int>, unsigned short> tables_taken_;

  void register_table(const std::string &name,
                      int shard_index,
                      abstract_ordered_index *idx) {
    const auto key = std::make_tuple(name, shard_index);
    if (tables_taken_.find(key) != tables_taken_.end()) {
      return;
    }
    const unsigned short table_id =
        static_cast<unsigned short>(tables_by_id_.size());
    tables_by_id_.push_back(idx);
    tables_taken_[key] = table_id;
  }
};


