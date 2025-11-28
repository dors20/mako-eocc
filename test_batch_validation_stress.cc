// Stress test for batch validation
// Creates many concurrent transactions to test batch validation performance

#include <iostream>
#include <iomanip>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <mako.hh>
#include "examples/common.h"

using namespace std;

atomic<uint64_t> committed_txns{0};
atomic<uint64_t> aborted_txns{0};
atomic<bool> stop_flag{false};

class StressWorker {
public:
    StressWorker(abstract_db *db, int worker_id, int num_txns) 
        : db(db), worker_id(worker_id), num_txns(num_txns) {
        txn_obj_buf.reserve(str_arena::MinStrReserveLength);
        txn_obj_buf.resize(db->sizeof_txn_object(0));
    }

    void initialize() {
        scoped_db_thread_ctx ctx(db, false);
        TThread::enable_multiverison();
        table = db->open_index("stress_table_" + to_string(worker_id % 4)); // 4 tables for contention
    }

    void run() {
        // Thread-local context required for Mako
        scoped_db_thread_ctx ctx(db, false);
        
        for (int i = 0; i < num_txns && !stop_flag.load(); i++) {
            void *txn = db->new_txn(0, arena, txn_buf());
            scoped_str_arena s_arena(arena);
            
            try {
                // Create contention: multiple threads accessing same keys
                string key = "key_" + to_string(i % 100); // Only 100 unique keys for contention
                string value = mako::Encode("value_" + to_string(worker_id) + "_" + to_string(i));
                
                // Write operation (creates contention)
                table->put(txn, key, value);
                
                // Sometimes read
                if (i % 3 == 0) {
                    string read_value;
                    table->get(txn, key, read_value);
                }
                
                db->commit_txn(txn);
                committed_txns.fetch_add(1, memory_order_relaxed);
            } catch (abstract_db::abstract_abort_exception &ex) {
                db->abort_txn(txn);
                aborted_txns.fetch_add(1, memory_order_relaxed);
            }
        }
    }

private:
    abstract_db *const db;
    int worker_id;
    int num_txns;
    str_arena arena;
    string txn_obj_buf;
    abstract_ordered_index *table;
    inline void *txn_buf() { return (void *)txn_obj_buf.data(); }
};

int main(int argc, char **argv) {
    int num_threads = 4;
    int txns_per_thread = 1000;
    
    if (argc > 1) num_threads = atoi(argv[1]);
    if (argc > 2) txns_per_thread = atoi(argv[2]);
    
    cout << "==========================================" << endl;
    cout << "Batch Validation Stress Test" << endl;
    cout << "==========================================" << endl;
    cout << "Threads: " << num_threads << endl;
    cout << "Transactions per thread: " << txns_per_thread << endl;
    cout << "Total transactions: " << (num_threads * txns_per_thread) << endl;
    cout << endl;
    
    // Check if batch validation is enabled
    const char* env = getenv("MAKO_ENABLE_BATCH_VALIDATION");
    if (env && (string(env) == "1" || string(env) == "true")) {
        cout << "✓ Batch validation: ENABLED" << endl;
        const char* batch_size = getenv("MAKO_BATCH_VALIDATION_SIZE");
        if (batch_size) {
            cout << "  Batch size: " << batch_size << endl;
        }
    } else {
        cout << "✗ Batch validation: DISABLED" << endl;
    }
    cout << endl;
    
    abstract_db *db = new mbta_wrapper;
    db->init();
    
    auto config = new transport::Configuration(
        get_current_absolute_path() + "../src/mako/config/local-shards2-warehouses1.yml"
    );
    BenchmarkConfig::getInstance().setConfig(config);
    
    vector<StressWorker*> workers;
    vector<thread> threads;
    
    // Create workers
    for (int i = 0; i < num_threads; i++) {
        workers.push_back(new StressWorker(db, i, txns_per_thread));
        workers[i]->initialize();
    }
    
    // Start benchmark
    auto start_time = chrono::high_resolution_clock::now();
    
    // Launch threads
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back([&workers, i]() {
            workers[i]->run();
        });
    }
    
    // Wait for all threads
    for (auto &t : threads) {
        t.join();
    }
    
    auto end_time = chrono::high_resolution_clock::now();
    auto duration = chrono::duration_cast<chrono::milliseconds>(end_time - start_time);
    
    // Cleanup
    for (auto *w : workers) {
        delete w;
    }
    delete db;
    
    // Print results
    uint64_t total_committed = committed_txns.load();
    uint64_t total_aborted = aborted_txns.load();
    uint64_t total_txns = total_committed + total_aborted;
    
    double duration_sec = duration.count() / 1000.0;
    double throughput = total_committed / duration_sec;
    double abort_rate = (total_aborted * 100.0) / total_txns;
    
    cout << "==========================================" << endl;
    cout << "Results" << endl;
    cout << "==========================================" << endl;
    cout << "Duration: " << duration.count() << " ms (" << duration_sec << " s)" << endl;
    cout << "Committed: " << total_committed << endl;
    cout << "Aborted: " << total_aborted << endl;
    cout << "Total: " << total_txns << endl;
    cout << "Abort Rate: " << abort_rate << "%" << endl;
    cout << "Throughput: " << fixed << setprecision(0) << throughput << " txns/sec" << endl;
    cout << endl;
    
    return 0;
}

