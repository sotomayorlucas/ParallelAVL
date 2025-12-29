#include <iostream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <thread>
#include <random>
#include <algorithm>
#include <numeric>
#include <fstream>

// Original implementation
#include "AVLTreeParallel.h"

// New implementation with dynamic scaling
#include "parallel_avl.hpp"

// Also test DynamicShardedTree directly
#include "DynamicShardedTree.hpp"

using namespace std::chrono;

// ============================================================================
// Benchmark Configuration
// ============================================================================

struct BenchConfig {
    size_t num_elements = 100000;
    size_t num_threads = 4;
    size_t num_shards = 8;
    size_t num_iterations = 3;
    bool verbose = true;
};

// ============================================================================
// Timer Utility
// ============================================================================

class Timer {
    high_resolution_clock::time_point start_;
public:
    Timer() : start_(high_resolution_clock::now()) {}
    
    double elapsed_ms() const {
        auto end = high_resolution_clock::now();
        return duration_cast<microseconds>(end - start_).count() / 1000.0;
    }
    
    void reset() { start_ = high_resolution_clock::now(); }
};

// ============================================================================
// Benchmark Results
// ============================================================================

struct BenchResult {
    std::string name;
    double insert_ms;
    double contains_ms;
    double remove_ms;
    double mixed_ops_per_sec;
    size_t final_size;
    double balance_score;
};

// ============================================================================
// Benchmark Functions
// ============================================================================

// Benchmark AVLTreeParallel (original)
BenchResult benchmark_original(const BenchConfig& config) {
    BenchResult result;
    result.name = "AVLTreeParallel (Original)";
    
    AVLTreeParallel<int, int> tree(config.num_shards);
    
    // Prepare data
    std::vector<int> keys(config.num_elements);
    std::iota(keys.begin(), keys.end(), 0);
    std::mt19937 rng(42);
    std::shuffle(keys.begin(), keys.end(), rng);
    
    // Benchmark INSERT
    Timer t;
    for (int key : keys) {
        tree.insert(key, key * 10);
    }
    result.insert_ms = t.elapsed_ms();
    
    // Benchmark CONTAINS
    std::shuffle(keys.begin(), keys.end(), rng);
    t.reset();
    size_t found = 0;
    for (int key : keys) {
        if (tree.contains(key)) found++;
    }
    result.contains_ms = t.elapsed_ms();
    
    // Benchmark REMOVE (50% of elements)
    t.reset();
    for (size_t i = 0; i < keys.size() / 2; ++i) {
        tree.remove(keys[i]);
    }
    result.remove_ms = t.elapsed_ms();
    
    // Mixed workload benchmark
    t.reset();
    size_t ops = 0;
    for (size_t i = 0; i < config.num_elements; ++i) {
        int key = rng() % config.num_elements;
        switch (i % 3) {
            case 0: tree.insert(key, key); break;
            case 1: tree.contains(key); break;
            case 2: tree.remove(key); break;
        }
        ops++;
    }
    double mixed_ms = t.elapsed_ms();
    result.mixed_ops_per_sec = (ops / mixed_ms) * 1000.0;
    
    result.final_size = tree.size();
    auto info = tree.getArchitectureInfo();
    result.balance_score = info.load_balance_score;
    
    return result;
}

// Benchmark ParallelAVL (new with dynamic scaling)
BenchResult benchmark_new(const BenchConfig& config) {
    BenchResult result;
    result.name = "ParallelAVL (New + Dynamic)";
    
    ParallelAVL<int, int> tree(config.num_shards);
    
    // Prepare data
    std::vector<int> keys(config.num_elements);
    std::iota(keys.begin(), keys.end(), 0);
    std::mt19937 rng(42);
    std::shuffle(keys.begin(), keys.end(), rng);
    
    // Benchmark INSERT
    Timer t;
    for (int key : keys) {
        tree.insert(key, key * 10);
    }
    result.insert_ms = t.elapsed_ms();
    
    // Benchmark CONTAINS
    std::shuffle(keys.begin(), keys.end(), rng);
    t.reset();
    size_t found = 0;
    for (int key : keys) {
        if (tree.contains(key)) found++;
    }
    result.contains_ms = t.elapsed_ms();
    
    // Benchmark REMOVE (50% of elements)
    t.reset();
    for (size_t i = 0; i < keys.size() / 2; ++i) {
        tree.remove(keys[i]);
    }
    result.remove_ms = t.elapsed_ms();
    
    // Mixed workload benchmark
    t.reset();
    size_t ops = 0;
    for (size_t i = 0; i < config.num_elements; ++i) {
        int key = rng() % config.num_elements;
        switch (i % 3) {
            case 0: tree.insert(key, key); break;
            case 1: tree.contains(key); break;
            case 2: tree.remove(key); break;
        }
        ops++;
    }
    double mixed_ms = t.elapsed_ms();
    result.mixed_ops_per_sec = (ops / mixed_ms) * 1000.0;
    
    result.final_size = tree.size();
    result.balance_score = tree.get_balance_score();
    
    return result;
}

// Benchmark DynamicShardedTree
BenchResult benchmark_dynamic_sharded(const BenchConfig& config) {
    BenchResult result;
    result.name = "DynamicShardedTree";
    
    typename DynamicShardedTree<int, int>::Config cfg;
    cfg.initial_shards = config.num_shards;
    DynamicShardedTree<int, int> tree(cfg);
    
    // Prepare data
    std::vector<int> keys(config.num_elements);
    std::iota(keys.begin(), keys.end(), 0);
    std::mt19937 rng(42);
    std::shuffle(keys.begin(), keys.end(), rng);
    
    // Benchmark INSERT
    Timer t;
    for (int key : keys) {
        tree.insert(key, key * 10);
    }
    result.insert_ms = t.elapsed_ms();
    
    // Benchmark CONTAINS
    std::shuffle(keys.begin(), keys.end(), rng);
    t.reset();
    size_t found = 0;
    for (int key : keys) {
        if (tree.contains(key)) found++;
    }
    result.contains_ms = t.elapsed_ms();
    
    // Benchmark REMOVE (50% of elements)
    t.reset();
    for (size_t i = 0; i < keys.size() / 2; ++i) {
        tree.remove(keys[i]);
    }
    result.remove_ms = t.elapsed_ms();
    
    // Mixed workload benchmark
    t.reset();
    size_t ops = 0;
    for (size_t i = 0; i < config.num_elements; ++i) {
        int key = rng() % config.num_elements;
        switch (i % 3) {
            case 0: tree.insert(key, key); break;
            case 1: tree.contains(key); break;
            case 2: tree.remove(key); break;
        }
        ops++;
    }
    double mixed_ms = t.elapsed_ms();
    result.mixed_ops_per_sec = (ops / mixed_ms) * 1000.0;
    
    result.final_size = tree.size();
    auto stats = tree.get_stats();
    result.balance_score = stats.balance_score;
    
    return result;
}

// ============================================================================
// Dynamic Scaling Benchmark
// ============================================================================

struct ScalingResult {
    double add_shard_ms;
    double remove_shard_ms;
    double rebalance_ms;
    bool data_integrity;
};

ScalingResult benchmark_dynamic_scaling(const BenchConfig& config) {
    ScalingResult result;
    
    ParallelAVL<int, int> tree(4);
    
    // Insert data
    for (size_t i = 0; i < config.num_elements; ++i) {
        tree.insert(i, i * 10);
    }
    
    // Benchmark add_shard
    Timer t;
    tree.add_shard();
    tree.add_shard();
    result.add_shard_ms = t.elapsed_ms();
    
    // Benchmark remove_shard
    t.reset();
    tree.remove_shard();
    result.remove_shard_ms = t.elapsed_ms();
    
    // Benchmark force_rebalance
    t.reset();
    tree.force_rebalance();
    result.rebalance_ms = t.elapsed_ms();
    
    // Verify data integrity
    result.data_integrity = true;
    for (size_t i = 0; i < config.num_elements; ++i) {
        if (!tree.contains(i)) {
            result.data_integrity = false;
            break;
        }
    }
    
    return result;
}

// ============================================================================
// Multi-threaded Benchmark
// ============================================================================

template<typename Tree>
double benchmark_multithreaded(Tree& tree, const BenchConfig& config) {
    std::vector<std::thread> threads;
    std::atomic<size_t> total_ops{0};
    
    size_t ops_per_thread = config.num_elements / config.num_threads;
    
    Timer t;
    
    for (size_t tid = 0; tid < config.num_threads; ++tid) {
        threads.emplace_back([&, tid]() {
            std::mt19937 rng(tid * 1000);
            size_t ops = 0;
            
            for (size_t i = 0; i < ops_per_thread; ++i) {
                int key = rng() % config.num_elements;
                switch (i % 3) {
                    case 0: tree.insert(key, key); break;
                    case 1: tree.contains(key); break;
                    case 2: tree.remove(key); break;
                }
                ops++;
            }
            
            total_ops.fetch_add(ops, std::memory_order_relaxed);
        });
    }
    
    for (auto& th : threads) {
        th.join();
    }
    
    double elapsed_ms = t.elapsed_ms();
    return (total_ops.load() / elapsed_ms) * 1000.0;  // ops/sec
}

// ============================================================================
// Print Results
// ============================================================================

void print_header() {
    std::cout << "\n";
    std::cout << "╔═══════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║           PARALLEL AVL TREE - PERFORMANCE COMPARISON                      ║\n";
    std::cout << "╚═══════════════════════════════════════════════════════════════════════════╝\n";
    std::cout << "\n";
}

void print_result(const BenchResult& r, const BenchResult* baseline = nullptr) {
    std::cout << "┌─────────────────────────────────────────────────────────────────┐\n";
    std::cout << "│ " << std::left << std::setw(63) << r.name << " │\n";
    std::cout << "├─────────────────────────────────────────────────────────────────┤\n";
    
    auto print_metric = [&](const char* name, double value, double base_value = 0) {
        std::cout << "│   " << std::left << std::setw(20) << name << ": " 
                  << std::right << std::setw(10) << std::fixed << std::setprecision(2) << value;
        
        if (baseline && base_value > 0) {
            double speedup = base_value / value;
            const char* indicator = speedup >= 1.0 ? "▲" : "▼";
            std::cout << " ms  (" << indicator << " " << std::setprecision(1) 
                      << std::abs(speedup - 1.0) * 100 << "%)";
        } else {
            std::cout << " ms";
        }
        std::cout << std::setw(15) << "" << "│\n";
    };
    
    print_metric("Insert", r.insert_ms, baseline ? baseline->insert_ms : 0);
    print_metric("Contains", r.contains_ms, baseline ? baseline->contains_ms : 0);
    print_metric("Remove", r.remove_ms, baseline ? baseline->remove_ms : 0);
    
    std::cout << "│   " << std::left << std::setw(20) << "Mixed Ops/sec" << ": " 
              << std::right << std::setw(10) << std::fixed << std::setprecision(0) 
              << r.mixed_ops_per_sec;
    
    if (baseline) {
        double speedup = r.mixed_ops_per_sec / baseline->mixed_ops_per_sec;
        const char* indicator = speedup >= 1.0 ? "▲" : "▼";
        std::cout << " ops/s (" << indicator << " " << std::setprecision(1) 
                  << std::abs(speedup - 1.0) * 100 << "%)";
    } else {
        std::cout << " ops/s";
    }
    std::cout << std::setw(10) << "" << "│\n";
    
    std::cout << "│   " << std::left << std::setw(20) << "Balance Score" << ": " 
              << std::right << std::setw(10) << std::fixed << std::setprecision(1) 
              << (r.balance_score * 100) << " %" << std::setw(22) << "" << "│\n";
    
    std::cout << "└─────────────────────────────────────────────────────────────────┘\n\n";
}

void print_scaling_result(const ScalingResult& r) {
    std::cout << "┌─────────────────────────────────────────────────────────────────┐\n";
    std::cout << "│ Dynamic Scaling Performance                                     │\n";
    std::cout << "├─────────────────────────────────────────────────────────────────┤\n";
    std::cout << "│   " << std::left << std::setw(20) << "Add 2 Shards" << ": " 
              << std::right << std::setw(10) << std::fixed << std::setprecision(2) 
              << r.add_shard_ms << " ms" << std::setw(23) << "" << "│\n";
    std::cout << "│   " << std::left << std::setw(20) << "Remove 1 Shard" << ": " 
              << std::right << std::setw(10) << std::fixed << std::setprecision(2) 
              << r.remove_shard_ms << " ms" << std::setw(23) << "" << "│\n";
    std::cout << "│   " << std::left << std::setw(20) << "Force Rebalance" << ": " 
              << std::right << std::setw(10) << std::fixed << std::setprecision(2) 
              << r.rebalance_ms << " ms" << std::setw(23) << "" << "│\n";
    std::cout << "│   " << std::left << std::setw(20) << "Data Integrity" << ": " 
              << std::right << std::setw(10) << (r.data_integrity ? "PASS ✓" : "FAIL ✗")
              << std::setw(26) << "" << "│\n";
    std::cout << "└─────────────────────────────────────────────────────────────────┘\n\n";
}

void save_csv_results(const std::vector<BenchResult>& results, const BenchConfig& config) {
    std::ofstream csv("bench/comparison_results.csv");
    csv << "Implementation,Insert_ms,Contains_ms,Remove_ms,Mixed_ops_sec,Balance_score,Elements,Shards\n";
    
    for (const auto& r : results) {
        csv << r.name << "," 
            << r.insert_ms << "," 
            << r.contains_ms << "," 
            << r.remove_ms << ","
            << r.mixed_ops_per_sec << ","
            << r.balance_score << ","
            << config.num_elements << ","
            << config.num_shards << "\n";
    }
    
    std::cout << "Results saved to bench/comparison_results.csv\n";
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    BenchConfig config;
    
    // Parse arguments
    if (argc > 1) config.num_elements = std::stoul(argv[1]);
    if (argc > 2) config.num_shards = std::stoul(argv[2]);
    if (argc > 3) config.num_threads = std::stoul(argv[3]);
    
    print_header();
    
    std::cout << "Configuration:\n";
    std::cout << "  Elements: " << config.num_elements << "\n";
    std::cout << "  Shards: " << config.num_shards << "\n";
    std::cout << "  Threads: " << config.num_threads << "\n\n";
    
    std::vector<BenchResult> results;
    
    // Run benchmarks
    std::cout << "Running benchmarks...\n\n";
    
    std::cout << "═══════════════════════════════════════════════════════════════════\n";
    std::cout << " SINGLE-THREADED PERFORMANCE\n";
    std::cout << "═══════════════════════════════════════════════════════════════════\n\n";
    
    // Original (baseline)
    auto original = benchmark_original(config);
    results.push_back(original);
    print_result(original);
    
    // New ParallelAVL
    auto new_impl = benchmark_new(config);
    results.push_back(new_impl);
    print_result(new_impl, &original);
    
    // DynamicShardedTree
    auto dynamic = benchmark_dynamic_sharded(config);
    results.push_back(dynamic);
    print_result(dynamic, &original);
    
    // Dynamic scaling benchmark
    std::cout << "═══════════════════════════════════════════════════════════════════\n";
    std::cout << " DYNAMIC SCALING OVERHEAD\n";
    std::cout << "═══════════════════════════════════════════════════════════════════\n\n";
    
    auto scaling = benchmark_dynamic_scaling(config);
    print_scaling_result(scaling);
    
    // Multi-threaded benchmark
    std::cout << "═══════════════════════════════════════════════════════════════════\n";
    std::cout << " MULTI-THREADED PERFORMANCE (" << config.num_threads << " threads)\n";
    std::cout << "═══════════════════════════════════════════════════════════════════\n\n";
    
    {
        AVLTreeParallel<int, int> tree1(config.num_shards);
        double ops1 = benchmark_multithreaded(tree1, config);
        
        ParallelAVL<int, int> tree2(config.num_shards);
        double ops2 = benchmark_multithreaded(tree2, config);
        
        typename DynamicShardedTree<int, int>::Config cfg;
        cfg.initial_shards = config.num_shards;
        DynamicShardedTree<int, int> tree3(cfg);
        double ops3 = benchmark_multithreaded(tree3, config);
        
        std::cout << "┌─────────────────────────────────────────────────────────────────┐\n";
        std::cout << "│ Multi-threaded Throughput (ops/sec)                             │\n";
        std::cout << "├─────────────────────────────────────────────────────────────────┤\n";
        
        auto print_mt = [&](const char* name, double ops, double baseline) {
            double speedup = ops / baseline;
            const char* indicator = speedup >= 1.0 ? "▲" : "▼";
            std::cout << "│   " << std::left << std::setw(25) << name << ": " 
                      << std::right << std::setw(12) << std::fixed << std::setprecision(0) << ops
                      << " (" << indicator << " " << std::setprecision(1) 
                      << std::abs(speedup - 1.0) * 100 << "%)" << std::setw(6) << "" << "│\n";
        };
        
        std::cout << "│   " << std::left << std::setw(25) << "AVLTreeParallel" << ": " 
                  << std::right << std::setw(12) << std::fixed << std::setprecision(0) << ops1
                  << " (baseline)" << std::setw(6) << "" << "│\n";
        print_mt("ParallelAVL (New)", ops2, ops1);
        print_mt("DynamicShardedTree", ops3, ops1);
        
        std::cout << "└─────────────────────────────────────────────────────────────────┘\n\n";
    }
    
    // Summary
    std::cout << "═══════════════════════════════════════════════════════════════════\n";
    std::cout << " SUMMARY\n";
    std::cout << "═══════════════════════════════════════════════════════════════════\n\n";
    
    double insert_speedup = original.insert_ms / new_impl.insert_ms;
    double contains_speedup = original.contains_ms / new_impl.contains_ms;
    double mixed_speedup = new_impl.mixed_ops_per_sec / original.mixed_ops_per_sec;
    
    std::cout << "ParallelAVL vs AVLTreeParallel:\n";
    std::cout << "  Insert:   " << (insert_speedup >= 1.0 ? "+" : "") 
              << std::fixed << std::setprecision(1) << (insert_speedup - 1.0) * 100 << "%\n";
    std::cout << "  Contains: " << (contains_speedup >= 1.0 ? "+" : "") 
              << (contains_speedup - 1.0) * 100 << "%\n";
    std::cout << "  Mixed:    " << (mixed_speedup >= 1.0 ? "+" : "") 
              << (mixed_speedup - 1.0) * 100 << "%\n";
    std::cout << "\nNew Features (not in original):\n";
    std::cout << "  ✓ Dynamic add_shard(): " << scaling.add_shard_ms << " ms\n";
    std::cout << "  ✓ Dynamic remove_shard(): " << scaling.remove_shard_ms << " ms\n";
    std::cout << "  ✓ Force rebalance(): " << scaling.rebalance_ms << " ms\n";
    std::cout << "  ✓ Lazy migration after topology changes\n";
    std::cout << "\n";
    
    // Save results
    save_csv_results(results, config);
    
    return 0;
}
