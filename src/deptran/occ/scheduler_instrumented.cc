// Instrumented version of OCC scheduler for analysis
// Include this file instead of scheduler.cc when OCC_ANALYSIS_ENABLED is defined

#include "../__dep__.h"
#include "../scheduler.h"
#include "../config.h"
#include "tx.h"
#include "scheduler.h"
#include "occ_analysis.h"
#include <sstream>

namespace janus {

// Helper to extract keys from read/write sets for analysis
std::vector<std::string> extractKeysFromReadSet(const mdb::TxnOCC* txn) {
    std::vector<std::string> keys;
    // Extract keys from ver_check_read_
    for (const auto& it : txn->ver_check_read_) {
        Row* row = it.first.row;
        if (row && row->get_table()) {
            // Try to get key from row (simplified - may need adjustment based on actual Row API)
            std::stringstream ss;
            ss << row->get_table()->Name() << ":" << (void*)row;
            keys.push_back(ss.str());
        }
    }
    return keys;
}

std::vector<std::string> extractKeysFromWriteSet(const mdb::TxnOCC* txn) {
    std::vector<std::string> keys;
    // Extract keys from updates_
    for (const auto& it : txn->updates_) {
        Row* row = it.first;
        if (row && row->get_table()) {
            std::stringstream ss;
            ss << row->get_table()->Name() << ":" << (void*)row;
            keys.push_back(ss.str());
        }
    }
    return keys;
}

SchedulerOcc::SchedulerOcc() : SchedulerClassic() {
    mdb_txn_mgr_ = make_shared<mdb::TxnMgrOCC>();
}

mdb::Txn* SchedulerOcc::get_mdb_txn(const i64 tid) {
    mdb::Txn *txn = nullptr;
    auto it = mdb_txns_.find(tid);
    if (it == mdb_txns_.end()) {
        txn = mdb_txn_mgr_->start(tid);
        ((mdb::TxnOCC *) txn)->set_policy(mdb::OCC_LAZY);
        auto ret = mdb_txns_.insert(std::pair<i64, mdb::Txn *>(tid, txn));
        verify(ret.second);
    } else {
        txn = it->second;
    }
    verify(mdb_txn_mgr_->rtti() == mdb::symbol_t::TXN_OCC);
    verify(txn->rtti() == mdb::symbol_t::TXN_OCC);
    verify(txn != nullptr);
    return txn;
}

bool SchedulerOcc::DoPrepare(txnid_t tx_id) {
    auto& profiler = PerformanceProfiler::getInstance();
    auto& abort_tracker = AbortTracker::getInstance();
    auto& false_analyzer = FalseAbortAnalyzer::getInstance();
    
    // Start profiling validation
    profiler.startValidation(tx_id);
    
    auto tx_box = dynamic_pointer_cast<TxOcc>(GetOrCreateTx(tx_id));
    auto txn = (mdb::TxnOCC*) get_mdb_txn(tx_id);
    verify(txn != nullptr);
    verify(txn->outcome_ == symbol_t::NONE);
    verify(!txn->verified_);
    
    // Record read/write sets for false abort analysis
    auto read_set = extractKeysFromReadSet(txn);
    auto write_set = extractKeysFromWriteSet(txn);
    false_analyzer.recordReadWriteSets(tx_id, read_set, write_set);
    
    // Version check phase
    profiler.startVersionCheck(tx_id);
    bool version_check_passed = true;
    
    if (tx_box->is_leader_hint_ && !txn->version_check()) {
        profiler.endVersionCheck(tx_id);
        profiler.endValidation(tx_id, false);
        
        // Record abort
        abort_tracker.recordAbort(tx_id, OCCAbortReason::VERSION_CONFLICT, 
                                  AbortType::VERSION_STALE);
        
        Log_debug("txn: occ validation failed. id %" PRIx64 "site: %x", 
                  (int64_t)tx_id, (int)this->site_id_);
        txn->__debug_abort_ = 1;
        return false;
    }
    
    profiler.endVersionCheck(tx_id);
    version_check_passed = true;
    
    // Lock acquisition phase
    // Read locks
    profiler.startReadLock(tx_id);
    std::vector<std::string> conflicting_read_keys;
    txnid_t conflicting_read_txn = 0;
    
    for (auto &it : txn->ver_check_read_) {
        Row *row = it.first.row;
        auto *v_row = (VersionedRow *) row;
        Log_debug("r_lock row: %llx", row);
        
        if (!v_row->rlock_row_by(txn->id())) {
            profiler.endReadLock(tx_id);
            profiler.endValidation(tx_id, false);
            
            // Record abort due to read lock failure
            abort_tracker.recordAbort(tx_id, OCCAbortReason::READ_LOCK_FAILED,
                                     AbortType::LOCK_CONTENTION);
            
            // Try to identify conflicting transaction
            // (This is simplified - actual implementation may need more context)
            
            for (auto &lit : txn->locks_) {
                Row* r = lit.first;
                verify(r->rtti() == symbol_t::ROW_VERSIONED);
                auto vr = (VersionedRow *) r;
                vr->unlock_row_by(txn->id());
            }
            txn->locks_.clear();
            Log_debug("txn: occ read locks failed. id %" PRIx64 ", site: %x, is-leader: %d", 
                     (int64_t)tx_id, (int)this->site_id_, tx_box->is_leader_hint_);
            txn->__debug_abort_ = 1;
            return false;
        }
        insert_into_map(txn->locks_, row, -1);
    }
    profiler.endReadLock(tx_id);
    
    // Write locks
    profiler.startWriteLock(tx_id);
    std::vector<std::string> conflicting_write_keys;
    txnid_t conflicting_write_txn = 0;
    
    for (auto &it : txn->updates_) {
        Row *row = it.first;
        auto v_row = (VersionedRow *) row;
        Log_debug("w_lock row: %llx", row);
        
        if (!v_row->wlock_row_by(txn->id())) {
            profiler.endWriteLock(tx_id);
            profiler.endValidation(tx_id, false);
            
            // Record abort due to write lock failure
            abort_tracker.recordAbort(tx_id, OCCAbortReason::WRITE_LOCK_FAILED,
                                     AbortType::LOCK_CONTENTION);
            
            for (auto &lit : txn->locks_) {
                Row* r = lit.first;
                verify(r->rtti() == symbol_t::ROW_VERSIONED);
                auto vr = (VersionedRow *) row;
                vr->unlock_row_by(txn->id());
            }
            txn->locks_.clear();
            Log_debug("txn: occ write locks failed. id %" PRIx64 "site: %x", 
                     (int64_t)tx_id, (int)this->site_id_);
            txn->__debug_abort_ = 1;
            return false;
        }
        insert_into_map(txn->locks_, row, -1);
    }
    profiler.endWriteLock(tx_id);
    
    // Validation succeeded
    profiler.endValidation(tx_id, true);
    abort_tracker.recordCommit(tx_id);
    
    Log_debug("txn: %llx occ locks succeed.", (int64_t)tx_id);
    txn->__debug_abort_ = 0;
    txn->verified_ = true;
    
    return true;
}

void SchedulerOcc::DoCommit(Tx& tx) {
    // Original commit logic unchanged
    auto cmd_id_ = tx.tid_;
    auto mdb_txn_ = (mdb::TxnOCC*) get_mdb_txn(cmd_id_);
    
    verify(mdb_txn_ == RemoveMTxn(cmd_id_));
    
    auto txn = dynamic_cast<mdb::TxnOCC*>(mdb_txn_);
    if (txn->__debug_abort_) {
        Log_fatal("2pc commit request received after prepare failure for %" PRIx64,
                  tx.tid_);
    }
    verify(txn->outcome_ == symbol_t::NONE);
    verify(txn->verified_);
    
    for (auto &it : txn->inserts_) {
        it.table->insert(it.row);
    }
    for (auto it = txn->updates_.begin(); it != txn->updates_.end(); /* no ++it! */) {
        Row *row = it->first;
        verify(row->rtti() == mdb::ROW_VERSIONED);
        auto v_row = (VersionedRow *) row;
        const Table *tbl = row->get_table();
        if (tbl->rtti() == mdb::TBL_SNAPSHOT) {
            Row *new_row = row->copy();
            auto v_new_row = (VersionedRow *) new_row;
            
            auto it_end = txn->updates_.upper_bound(row);
            while (it != it_end) {
                colid_t column_id = it->second.first;
                Value &value = it->second.second;
                new_row->update(column_id, value);
                if (txn->policy_ == symbol_t::OCC_LAZY) {
                    v_row->incr_column_ver(column_id);
                    v_new_row->incr_column_ver(column_id);
                }
                ++it;
            }
            
            auto ss_tbl = (SnapshotTable *) tbl;
            ss_tbl->remove(row);
            ss_tbl->insert(new_row);
            
            redirect_locks(txn->locks_, new_row, row);
            
            auto it_accessed = txn->accessed_rows_.find(row);
            if (it_accessed != txn->accessed_rows_.end()) {
                (*it_accessed)->release();
                txn->accessed_rows_.erase(it_accessed);
                new_row->ref_copy();
                txn->accessed_rows_.insert(new_row);
            }
        } else {
            colid_t column_id = it->second.first;
            Value &value = it->second.second;
            row->update(column_id, value);
            if (txn->policy_ == symbol_t::OCC_LAZY) {
                v_row->incr_column_ver(column_id);
            }
            ++it;
        }
    }
    for (auto &it : txn->removes_) {
        if (txn->policy_ == symbol_t::OCC_LAZY) {
            Row *row = it.row;
            verify(row->rtti() == symbol_t::ROW_VERSIONED);
            auto v_row = (VersionedRow *) row;
            for (size_t col_id = 0; col_id < v_row->schema()->columns_count();
                 col_id++) {
                v_row->incr_column_ver(col_id);
            }
        }
        txn->locks_.erase(it.row);
        it.table->remove(it.row);
    }
    txn->outcome_ = symbol_t::TXN_COMMIT;
    txn->release_resource();
    delete mdb_txn_;
}

} // namespace janus

