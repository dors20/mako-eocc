#pragma once

#include <chrono>
#include <unordered_map>
#include <map>
#include <vector>
#include <atomic>
#include <mutex>
#include <string>
#include <fstream>
#include <iomanip>
#include "../__dep__.h"

namespace janus {

// Abort reasons specific to OCC
enum class OCCAbortReason {
    VERSION_CONFLICT,        // Version check failed
    READ_LOCK_FAILED,        // Failed to acquire read lock
    WRITE_LOCK_FAILED,       // Failed to acquire write lock
    LEADER_VERSION_CHECK,    // Version check failed on leader
    UNKNOWN
};

// Types of aborts for false abort analysis
enum class AbortType {
    REAL_CONFLICT,          // Actual serializability violation (must abort)
    FALSE_ABORT,            // Could be avoided with reordering/batching
    VERSION_STALE,          // Read stale version unnecessarily
    ORDERING_ISSUE,         // Wrong validation order
    LOCK_CONTENTION         // Lock contention (could use hybrid approach)
};

// Transaction execution phase timing
struct TransactionTiming {
    txnid_t txn_id;
    std::chrono::high_resolution_clock::time_point start_time;
    std::chrono::high_resolution_clock::time_point validation_start;
    std::chrono::high_resolution_clock::time_point validation_end;
    std::chrono::high_resolution_clock::time_point commit_time;
    
    // Breakdown of validation time
    uint64_t version_check_time_ns = 0;
    uint64_t read_lock_time_ns = 0;
    uint64_t write_lock_time_ns = 0;
    uint64_t total_validation_time_ns = 0;
    
    bool aborted = false;
    OCCAbortReason abort_reason = OCCAbortReason::UNKNOWN;
    AbortType abort_type = AbortType::REAL_CONFLICT;
};

// Conflict information for false abort analysis
struct ConflictInfo {
    txnid_t txn1_id;
    txnid_t txn2_id;
    std::vector<std::string> conflicting_keys;  // Keys that caused conflict
    bool could_reorder;  // Could reordering avoid this abort?
    std::string conflict_type;  // "read-write", "write-write", "write-read"
};

// Performance metrics
struct ValidationMetrics {
    // Timing statistics
    uint64_t total_validations = 0;
    uint64_t total_aborts = 0;
    uint64_t total_commits = 0;
    
    // Time measurements (nanoseconds)
    uint64_t total_validation_time_ns = 0;
    uint64_t total_version_check_time_ns = 0;
    uint64_t total_lock_time_ns = 0;
    
    // Abort statistics
    std::map<OCCAbortReason, uint64_t> abort_reason_counts;
    std::map<AbortType, uint64_t> abort_type_counts;
    
    // Read/write set sizes
    uint64_t total_read_set_size = 0;
    uint64_t total_write_set_size = 0;
    
    // Lock contention
    uint64_t read_lock_failures = 0;
    uint64_t write_lock_failures = 0;
    
    // False abort analysis
    uint64_t false_aborts = 0;
    std::vector<ConflictInfo> false_abort_conflicts;
};

/**
 * AbortTracker: Tracks abort statistics and reasons
 */
class AbortTracker {
public:
    static AbortTracker& getInstance() {
        static AbortTracker instance;
        return instance;
    }
    
    void recordAbort(txnid_t txn_id, OCCAbortReason reason, AbortType type = AbortType::REAL_CONFLICT);
    void recordCommit(txnid_t txn_id);
    void recordConflict(txnid_t txn1, txnid_t txn2, const std::vector<std::string>& keys, 
                       bool could_reorder, const std::string& conflict_type);
    
    uint32_t getAbortCount(txnid_t txn_id) const;
    void printStatistics() const;
    void writeStatisticsToFile(const std::string& filename) const;
    void reset();
    
    // Get metrics
    ValidationMetrics getMetrics() const { return metrics_; }
    
private:
    AbortTracker() = default;
    mutable std::mutex mutex_;
    std::map<txnid_t, uint32_t> txn_abort_counts_;
    ValidationMetrics metrics_;
    std::vector<ConflictInfo> conflicts_;
};

/**
 * PerformanceProfiler: Profiles validation phase performance
 */
class PerformanceProfiler {
public:
    static PerformanceProfiler& getInstance() {
        static PerformanceProfiler instance;
        return instance;
    }
    
    void startValidation(txnid_t txn_id);
    void startVersionCheck(txnid_t txn_id);
    void endVersionCheck(txnid_t txn_id);
    void startReadLock(txnid_t txn_id);
    void endReadLock(txnid_t txn_id);
    void startWriteLock(txnid_t txn_id);
    void endWriteLock(txnid_t txn_id);
    void endValidation(txnid_t txn_id, bool success);
    
    TransactionTiming* getTiming(txnid_t txn_id);
    void printStatistics() const;
    void writeStatisticsToFile(const std::string& filename) const;
    void reset();
    
    // Get metrics
    ValidationMetrics getMetrics() const;
    
private:
    PerformanceProfiler() = default;
    mutable std::mutex mutex_;
    std::unordered_map<txnid_t, TransactionTiming> timings_;
    std::chrono::high_resolution_clock::time_point version_check_start_;
    std::chrono::high_resolution_clock::time_point lock_start_;
};

/**
 * FalseAbortAnalyzer: Analyzes false aborts that could be avoided
 */
class FalseAbortAnalyzer {
public:
    static FalseAbortAnalyzer& getInstance() {
        static FalseAbortAnalyzer instance;
        return instance;
    }
    
    // Analyze if an abort could be avoided by reordering
    bool analyzeAbort(txnid_t aborted_txn, txnid_t conflicting_txn, 
                     const std::vector<std::string>& conflicting_keys);
    
    // Check if transactions could commit in different order
    bool couldReorder(txnid_t txn1, txnid_t txn2);
    
    // Get read/write sets for analysis
    void recordReadWriteSets(txnid_t txn_id, 
                            const std::vector<std::string>& read_set,
                            const std::vector<std::string>& write_set);
    
    void printAnalysis() const;
    void writeAnalysisToFile(const std::string& filename) const;
    void reset();
    
    uint64_t getFalseAbortCount() const { return false_abort_count_; }
    
private:
    FalseAbortAnalyzer() = default;
    mutable std::mutex mutex_;
    
    struct TransactionSets {
        std::vector<std::string> read_set;
        std::vector<std::string> write_set;
        std::chrono::high_resolution_clock::time_point start_time;
    };
    
    std::unordered_map<txnid_t, TransactionSets> txn_sets_;
    std::vector<ConflictInfo> false_aborts_;
    uint64_t false_abort_count_ = 0;
};

// Helper functions
inline uint64_t getTimeNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()
    ).count();
}

inline uint64_t getElapsedNs(
    const std::chrono::high_resolution_clock::time_point& start,
    const std::chrono::high_resolution_clock::time_point& end
) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
}

} // namespace janus

