#include "occ_analysis.h"
#include "../scheduler.h"
#include <sstream>
#include <algorithm>
#include <iomanip>

namespace janus {

// AbortTracker Implementation
void AbortTracker::recordAbort(txnid_t txn_id, OCCAbortReason reason, AbortType type) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    metrics_.total_aborts++;
    metrics_.total_validations++;
    metrics_.abort_reason_counts[reason]++;
    metrics_.abort_type_counts[type]++;
    
    if (type == AbortType::FALSE_ABORT || 
        type == AbortType::VERSION_STALE || 
        type == AbortType::ORDERING_ISSUE) {
        metrics_.false_aborts++;
    }
    
    txn_abort_counts_[txn_id]++;
}

void AbortTracker::recordCommit(txnid_t txn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    metrics_.total_commits++;
    metrics_.total_validations++;
}

void AbortTracker::recordConflict(txnid_t txn1, txnid_t txn2, 
                                  const std::vector<std::string>& keys,
                                  bool could_reorder,
                                  const std::string& conflict_type) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    ConflictInfo info;
    info.txn1_id = txn1;
    info.txn2_id = txn2;
    info.conflicting_keys = keys;
    info.could_reorder = could_reorder;
    info.conflict_type = conflict_type;
    
    conflicts_.push_back(info);
    
    if (could_reorder) {
        metrics_.false_aborts++;
    }
}

uint32_t AbortTracker::getAbortCount(txnid_t txn_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = txn_abort_counts_.find(txn_id);
    return (it != txn_abort_counts_.end()) ? it->second : 0;
}

void AbortTracker::printStatistics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::cout << "\n========== OCC Abort Statistics ==========\n";
    std::cout << "Total Validations: " << metrics_.total_validations << "\n";
    std::cout << "Total Commits: " << metrics_.total_commits << "\n";
    std::cout << "Total Aborts: " << metrics_.total_aborts << "\n";
    
    if (metrics_.total_validations > 0) {
        double abort_rate = (double)metrics_.total_aborts / metrics_.total_validations * 100.0;
        std::cout << "Abort Rate: " << std::fixed << std::setprecision(2) << abort_rate << "%\n";
    }
    
    std::cout << "\nAbort Reasons:\n";
    for (const auto& [reason, count] : metrics_.abort_reason_counts) {
        std::cout << "  " << static_cast<int>(reason) << ": " << count << "\n";
    }
    
    std::cout << "\nAbort Types:\n";
    auto get_count = [&](AbortType type) -> uint64_t {
        auto it = metrics_.abort_type_counts.find(type);
        return (it != metrics_.abort_type_counts.end()) ? it->second : 0;
    };
    std::cout << "  Real Conflicts: " << get_count(AbortType::REAL_CONFLICT) << "\n";
    std::cout << "  False Aborts: " << get_count(AbortType::FALSE_ABORT) << "\n";
    std::cout << "  Version Stale: " << get_count(AbortType::VERSION_STALE) << "\n";
    std::cout << "  Ordering Issues: " << get_count(AbortType::ORDERING_ISSUE) << "\n";
    std::cout << "  Lock Contention: " << get_count(AbortType::LOCK_CONTENTION) << "\n";
    
    std::cout << "\nFalse Abort Rate: " << metrics_.false_aborts << " / " << metrics_.total_aborts;
    if (metrics_.total_aborts > 0) {
        double false_abort_rate = (double)metrics_.false_aborts / metrics_.total_aborts * 100.0;
        std::cout << " (" << std::fixed << std::setprecision(2) << false_abort_rate << "%)\n";
    } else {
        std::cout << "\n";
    }
    
    std::cout << "==========================================\n\n";
}

void AbortTracker::writeStatisticsToFile(const std::string& filename) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::ofstream file(filename);
    if (!file.is_open()) {
        Log_error("Failed to open file for writing statistics: %s", filename.c_str());
        return;
    }
    
    file << "OCC Abort Statistics\n";
    file << "====================\n\n";
    file << "Total Validations: " << metrics_.total_validations << "\n";
    file << "Total Commits: " << metrics_.total_commits << "\n";
    file << "Total Aborts: " << metrics_.total_aborts << "\n";
    
    if (metrics_.total_validations > 0) {
        double abort_rate = (double)metrics_.total_aborts / metrics_.total_validations * 100.0;
        file << "Abort Rate: " << std::fixed << std::setprecision(2) << abort_rate << "%\n";
    }
    
    file << "\nAbort Reasons:\n";
    for (const auto& [reason, count] : metrics_.abort_reason_counts) {
        file << "  Reason " << static_cast<int>(reason) << ": " << count << "\n";
    }
    
    file << "\nAbort Types:\n";
    auto get_count = [&](AbortType type) -> uint64_t {
        auto it = metrics_.abort_type_counts.find(type);
        return (it != metrics_.abort_type_counts.end()) ? it->second : 0;
    };
    file << "  Real Conflicts: " << get_count(AbortType::REAL_CONFLICT) << "\n";
    file << "  False Aborts: " << get_count(AbortType::FALSE_ABORT) << "\n";
    file << "  Version Stale: " << get_count(AbortType::VERSION_STALE) << "\n";
    file << "  Ordering Issues: " << get_count(AbortType::ORDERING_ISSUE) << "\n";
    file << "  Lock Contention: " << get_count(AbortType::LOCK_CONTENTION) << "\n";
    
    file << "\nConflicts:\n";
    for (const auto& conflict : conflicts_) {
        file << "  Txn " << conflict.txn1_id << " <-> Txn " << conflict.txn2_id;
        file << " (" << conflict.conflict_type << ")";
        if (conflict.could_reorder) {
            file << " [COULD REORDER]";
        }
        file << "\n";
        file << "    Keys: ";
        for (const auto& key : conflict.conflicting_keys) {
            file << key << " ";
        }
        file << "\n";
    }
    
    file.close();
}

void AbortTracker::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    metrics_ = ValidationMetrics();
    txn_abort_counts_.clear();
    conflicts_.clear();
}

// PerformanceProfiler Implementation
void PerformanceProfiler::startValidation(txnid_t txn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& timing = timings_[txn_id];
    timing.txn_id = txn_id;
    timing.start_time = std::chrono::high_resolution_clock::now();
    timing.validation_start = timing.start_time;
}

void PerformanceProfiler::startVersionCheck(txnid_t txn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    version_check_start_ = std::chrono::high_resolution_clock::now();
}

void PerformanceProfiler::endVersionCheck(txnid_t txn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed = getElapsedNs(version_check_start_, end);
    
    auto it = timings_.find(txn_id);
    if (it != timings_.end()) {
        it->second.version_check_time_ns = elapsed;
    }
}

void PerformanceProfiler::startReadLock(txnid_t txn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    lock_start_ = std::chrono::high_resolution_clock::now();
}

void PerformanceProfiler::endReadLock(txnid_t txn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed = getElapsedNs(lock_start_, end);
    
    auto it = timings_.find(txn_id);
    if (it != timings_.end()) {
        it->second.read_lock_time_ns += elapsed;
    }
}

void PerformanceProfiler::startWriteLock(txnid_t txn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    lock_start_ = std::chrono::high_resolution_clock::now();
}

void PerformanceProfiler::endWriteLock(txnid_t txn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed = getElapsedNs(lock_start_, end);
    
    auto it = timings_.find(txn_id);
    if (it != timings_.end()) {
        it->second.write_lock_time_ns += elapsed;
    }
}

void PerformanceProfiler::endValidation(txnid_t txn_id, bool success) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto end = std::chrono::high_resolution_clock::now();
    
    auto it = timings_.find(txn_id);
    if (it != timings_.end()) {
        it->second.validation_end = end;
        it->second.total_validation_time_ns = getElapsedNs(
            it->second.validation_start, end
        );
        it->second.aborted = !success;
    }
}

TransactionTiming* PerformanceProfiler::getTiming(txnid_t txn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = timings_.find(txn_id);
    return (it != timings_.end()) ? &it->second : nullptr;
}

void PerformanceProfiler::printStatistics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (timings_.empty()) {
        std::cout << "No timing data collected.\n";
        return;
    }
    
    // Calculate statistics
    std::vector<uint64_t> validation_times;
    std::vector<uint64_t> version_check_times;
    std::vector<uint64_t> lock_times;
    
    for (const auto& [txn_id, timing] : timings_) {
        validation_times.push_back(timing.total_validation_time_ns);
        version_check_times.push_back(timing.version_check_time_ns);
        lock_times.push_back(timing.read_lock_time_ns + timing.write_lock_time_ns);
    }
    
    std::sort(validation_times.begin(), validation_times.end());
    std::sort(version_check_times.begin(), version_check_times.end());
    std::sort(lock_times.begin(), lock_times.end());
    
    auto percentile = [](const std::vector<uint64_t>& vec, double p) -> uint64_t {
        if (vec.empty()) return 0;
        size_t idx = static_cast<size_t>(vec.size() * p);
        return vec[std::min(idx, vec.size() - 1)];
    };
    
    std::cout << "\n========== OCC Performance Statistics ==========\n";
    std::cout << "Total Transactions: " << timings_.size() << "\n\n";
    
    std::cout << "Validation Time (ns):\n";
    std::cout << "  P50: " << percentile(validation_times, 0.50) << "\n";
    std::cout << "  P95: " << percentile(validation_times, 0.95) << "\n";
    std::cout << "  P99: " << percentile(validation_times, 0.99) << "\n";
    std::cout << "  Avg: " << (validation_times.empty() ? 0 : 
        std::accumulate(validation_times.begin(), validation_times.end(), 0ULL) / validation_times.size()) << "\n";
    
    std::cout << "\nVersion Check Time (ns):\n";
    std::cout << "  P50: " << percentile(version_check_times, 0.50) << "\n";
    std::cout << "  P95: " << percentile(version_check_times, 0.95) << "\n";
    std::cout << "  P99: " << percentile(version_check_times, 0.99) << "\n";
    
    std::cout << "\nLock Acquisition Time (ns):\n";
    std::cout << "  P50: " << percentile(lock_times, 0.50) << "\n";
    std::cout << "  P95: " << percentile(lock_times, 0.95) << "\n";
    std::cout << "  P99: " << percentile(lock_times, 0.99) << "\n";
    
    std::cout << "================================================\n\n";
}

void PerformanceProfiler::writeStatisticsToFile(const std::string& filename) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::ofstream file(filename);
    if (!file.is_open()) {
        Log_error("Failed to open file for writing performance stats: %s", filename.c_str());
        return;
    }
    
    // Write CSV format
    file << "txn_id,validation_time_ns,version_check_time_ns,read_lock_time_ns,write_lock_time_ns,aborted\n";
    
    for (const auto& [txn_id, timing] : timings_) {
        file << txn_id << ","
             << timing.total_validation_time_ns << ","
             << timing.version_check_time_ns << ","
             << timing.read_lock_time_ns << ","
             << timing.write_lock_time_ns << ","
             << (timing.aborted ? 1 : 0) << "\n";
    }
    
    file.close();
}

void PerformanceProfiler::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    timings_.clear();
}

ValidationMetrics PerformanceProfiler::getMetrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    ValidationMetrics metrics;
    metrics.total_validations = timings_.size();
    
    for (const auto& [txn_id, timing] : timings_) {
        metrics.total_validation_time_ns += timing.total_validation_time_ns;
        metrics.total_version_check_time_ns += timing.version_check_time_ns;
        metrics.total_lock_time_ns += timing.read_lock_time_ns + timing.write_lock_time_ns;
        
        if (timing.aborted) {
            metrics.total_aborts++;
        } else {
            metrics.total_commits++;
        }
    }
    
    return metrics;
}

// FalseAbortAnalyzer Implementation
bool FalseAbortAnalyzer::analyzeAbort(txnid_t aborted_txn, txnid_t conflicting_txn,
                                     const std::vector<std::string>& conflicting_keys) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it1 = txn_sets_.find(aborted_txn);
    auto it2 = txn_sets_.find(conflicting_txn);
    
    if (it1 == txn_sets_.end() || it2 == txn_sets_.end()) {
        return false;  // Don't have enough info
    }
    
    const auto& sets1 = it1->second;
    const auto& sets2 = it2->second;
    
    // Check if reordering could avoid abort
    // If txn1 reads what txn2 writes, and txn2 doesn't read what txn1 writes,
    // then txn1 could commit before txn2
    bool could_reorder = false;
    
    // Check for read-write dependency
    bool txn1_reads_txn2_writes = false;
    bool txn2_reads_txn1_writes = false;
    
    for (const auto& key : conflicting_keys) {
        // Check if aborted_txn reads what conflicting_txn writes
        bool aborted_reads = std::find(sets1.read_set.begin(), sets1.read_set.end(), key) != sets1.read_set.end();
        bool conflicting_writes = std::find(sets2.write_set.begin(), sets2.write_set.end(), key) != sets2.write_set.end();
        
        if (aborted_reads && conflicting_writes) {
            txn1_reads_txn2_writes = true;
        }
        
        // Check if conflicting_txn reads what aborted_txn writes
        bool conflicting_reads = std::find(sets2.read_set.begin(), sets2.read_set.end(), key) != sets2.read_set.end();
        bool aborted_writes = std::find(sets1.write_set.begin(), sets1.write_set.end(), key) != sets1.write_set.end();
        
        if (conflicting_reads && aborted_writes) {
            txn2_reads_txn1_writes = true;
        }
    }
    
    // If only one-way dependency, could reorder
    if (txn1_reads_txn2_writes && !txn2_reads_txn1_writes) {
        could_reorder = true;  // Could commit conflicting_txn first
    }
    
    if (could_reorder) {
        false_abort_count_++;
        
        ConflictInfo info;
        info.txn1_id = aborted_txn;
        info.txn2_id = conflicting_txn;
        info.conflicting_keys = conflicting_keys;
        info.could_reorder = true;
        info.conflict_type = txn1_reads_txn2_writes ? "read-write" : "write-read";
        
        false_aborts_.push_back(info);
    }
    
    return could_reorder;
}

bool FalseAbortAnalyzer::couldReorder(txnid_t txn1, txnid_t txn2) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it1 = txn_sets_.find(txn1);
    auto it2 = txn_sets_.find(txn2);
    
    if (it1 == txn_sets_.end() || it2 == txn_sets_.end()) {
        return false;
    }
    
    const auto& sets1 = it1->second;
    const auto& sets2 = it2->second;
    
    // Check if there's a two-way dependency (can't reorder)
    bool txn1_reads_txn2_writes = false;
    bool txn2_reads_txn1_writes = false;
    
    for (const auto& key : sets1.read_set) {
        if (std::find(sets2.write_set.begin(), sets2.write_set.end(), key) != sets2.write_set.end()) {
            txn1_reads_txn2_writes = true;
            break;
        }
    }
    
    for (const auto& key : sets2.read_set) {
        if (std::find(sets1.write_set.begin(), sets1.write_set.end(), key) != sets1.write_set.end()) {
            txn2_reads_txn1_writes = true;
            break;
        }
    }
    
    // Can reorder if only one-way dependency
    return (txn1_reads_txn2_writes && !txn2_reads_txn1_writes) ||
           (txn2_reads_txn1_writes && !txn1_reads_txn2_writes);
}

void FalseAbortAnalyzer::recordReadWriteSets(txnid_t txn_id,
                                             const std::vector<std::string>& read_set,
                                             const std::vector<std::string>& write_set) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    TransactionSets& sets = txn_sets_[txn_id];
    sets.read_set = read_set;
    sets.write_set = write_set;
    sets.start_time = std::chrono::high_resolution_clock::now();
}

void FalseAbortAnalyzer::printAnalysis() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::cout << "\n========== False Abort Analysis ==========\n";
    std::cout << "Total False Aborts Detected: " << false_abort_count_ << "\n";
    std::cout << "Total Conflicts Analyzed: " << false_aborts_.size() << "\n\n";
    
    std::cout << "False Abort Details:\n";
    for (size_t i = 0; i < std::min(size_t(10), false_aborts_.size()); i++) {
        const auto& abort = false_aborts_[i];
        std::cout << "  " << (i+1) << ". Txn " << abort.txn1_id << " <-> Txn " << abort.txn2_id;
        std::cout << " (" << abort.conflict_type << ")\n";
        std::cout << "     Keys: ";
        for (const auto& key : abort.conflicting_keys) {
            std::cout << key << " ";
        }
        std::cout << "\n";
    }
    
    if (false_aborts_.size() > 10) {
        std::cout << "  ... (" << (false_aborts_.size() - 10) << " more)\n";
    }
    
    std::cout << "==========================================\n\n";
}

void FalseAbortAnalyzer::writeAnalysisToFile(const std::string& filename) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::ofstream file(filename);
    if (!file.is_open()) {
        Log_error("Failed to open file for writing false abort analysis: %s", filename.c_str());
        return;
    }
    
    file << "False Abort Analysis\n";
    file << "====================\n\n";
    file << "Total False Aborts: " << false_abort_count_ << "\n\n";
    
    file << "txn1_id,txn2_id,conflict_type,could_reorder,conflicting_keys\n";
    for (const auto& abort : false_aborts_) {
        file << abort.txn1_id << "," << abort.txn2_id << "," << abort.conflict_type << ",";
        file << (abort.could_reorder ? "true" : "false") << ",";
        for (size_t i = 0; i < abort.conflicting_keys.size(); i++) {
            if (i > 0) file << ";";
            file << abort.conflicting_keys[i];
        }
        file << "\n";
    }
    
    file.close();
}

void FalseAbortAnalyzer::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    txn_sets_.clear();
    false_aborts_.clear();
    false_abort_count_ = 0;
}

} // namespace janus

