/**
 * Heavy Benchmark using the REAL ParallelAVL implementation
 * Tests the fix for router counting bug
 */

#include "../include/parallel_avl.hpp"
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <random>
#include <iomanip>
#include <cmath>

class HeavyBenchmark {
private:
    static constexpr size_t WARMUP_OPS = 10000;
    
    struct BenchResult {
        double throughput_ops_sec;
        double balance_score;
        double latency_us;
        size_t total_elements;
        bool has_hotspot;
    };

    template<typename Func>
    double measure_time_ms(Func&& f) {
        auto start = std::chrono::high_resolution_clock::now();
        f();
        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(end - start).count();
    }

public:
    BenchResult run_insert_benchmark(
        size_t num_threads,
        size_t ops_per_thread,
        size_t num_shards,
        ParallelAVL<int, int>::RouterStrategy strategy
    ) {
        ParallelAVL<int, int> tree(num_shards, strategy);
        
        std::vector<std::thread> threads;
        std::atomic<size_t> total_ops{0};
        
        auto duration = measure_time_ms([&]() {
            for (size_t t = 0; t < num_threads; ++t) {
                threads.emplace_back([&, t]() {
                    std::mt19937 rng(t * 12345);
                    std::uniform_int_distribution<int> dist(0, 10000000);
                    
                    for (size_t i = 0; i < ops_per_thread; ++i) {
                        int key = dist(rng);
                        tree.insert(key, key);
                        total_ops.fetch_add(1, std::memory_order_relaxed);
                    }
                });
            }
            for (auto& t : threads) t.join();
        });
        
        auto stats = tree.get_stats();
        
        BenchResult result;
        result.throughput_ops_sec = (total_ops.load() / duration) * 1000.0;
        result.balance_score = stats.balance_score;
        result.latency_us = (duration * 1000.0) / total_ops.load();
        result.total_elements = stats.total_size;
        result.has_hotspot = stats.has_hotspot;
        
        return result;
    }

    BenchResult run_mixed_benchmark(
        size_t num_threads,
        size_t ops_per_thread,
        size_t num_shards,
        ParallelAVL<int, int>::RouterStrategy strategy,
        double read_ratio = 0.8
    ) {
        ParallelAVL<int, int> tree(num_shards, strategy);
        
        // Pre-populate
        for (int i = 0; i < 100000; ++i) {
            tree.insert(i, i);
        }
        
        std::vector<std::thread> threads;
        std::atomic<size_t> total_ops{0};
        
        auto duration = measure_time_ms([&]() {
            for (size_t t = 0; t < num_threads; ++t) {
                threads.emplace_back([&, t]() {
                    std::mt19937 rng(t * 12345);
                    std::uniform_int_distribution<int> key_dist(0, 200000);
                    std::uniform_real_distribution<double> op_dist(0.0, 1.0);
                    
                    for (size_t i = 0; i < ops_per_thread; ++i) {
                        int key = key_dist(rng);
                        if (op_dist(rng) < read_ratio) {
                            tree.contains(key);
                        } else {
                            tree.insert(key, key);
                        }
                        total_ops.fetch_add(1, std::memory_order_relaxed);
                    }
                });
            }
            for (auto& t : threads) t.join();
        });
        
        auto stats = tree.get_stats();
        
        BenchResult result;
        result.throughput_ops_sec = (total_ops.load() / duration) * 1000.0;
        result.balance_score = stats.balance_score;
        result.latency_us = (duration * 1000.0) / total_ops.load();
        result.total_elements = stats.total_size;
        result.has_hotspot = stats.has_hotspot;
        
        return result;
    }

    void run_all() {
        std::cout << "\n";
        std::cout << "================================================================================\n";
        std::cout << "                    HEAVY BENCHMARK - ParallelAVL with FIX\n";
        std::cout << "================================================================================\n\n";

        const std::vector<std::pair<std::string, ParallelAVL<int, int>::RouterStrategy>> strategies = {
            {"Static Hash", ParallelAVL<int, int>::RouterStrategy::STATIC_HASH},
            {"Load-Aware", ParallelAVL<int, int>::RouterStrategy::LOAD_AWARE},
            {"Intelligent", ParallelAVL<int, int>::RouterStrategy::INTELLIGENT},
        };

        // Test 1: Thread Scaling
        std::cout << "=== Test 1: Thread Scaling (8 shards, 100K ops/thread) ===\n\n";
        std::cout << std::setw(12) << "Strategy" 
                  << std::setw(10) << "Threads"
                  << std::setw(15) << "Throughput"
                  << std::setw(12) << "Balance"
                  << std::setw(12) << "Latency"
                  << std::setw(10) << "Hotspot" << "\n";
        std::cout << std::string(71, '-') << "\n";

        for (const auto& [name, strategy] : strategies) {
            for (size_t threads : {1, 2, 4, 8, 16}) {
                auto result = run_insert_benchmark(threads, 100000, 8, strategy);
                std::cout << std::setw(12) << name
                          << std::setw(10) << threads
                          << std::setw(12) << std::fixed << std::setprecision(0) 
                          << result.throughput_ops_sec << " ops/s"
                          << std::setw(10) << std::setprecision(1) 
                          << (result.balance_score * 100) << "%"
                          << std::setw(10) << std::setprecision(2) 
                          << result.latency_us << " us"
                          << std::setw(10) << (result.has_hotspot ? "YES" : "No") << "\n";
            }
            std::cout << "\n";
        }

        // Test 2: Shard Scaling
        std::cout << "\n=== Test 2: Shard Scaling (8 threads, 100K ops/thread) ===\n\n";
        std::cout << std::setw(12) << "Strategy" 
                  << std::setw(10) << "Shards"
                  << std::setw(15) << "Throughput"
                  << std::setw(12) << "Balance"
                  << std::setw(12) << "Elements" << "\n";
        std::cout << std::string(61, '-') << "\n";

        for (const auto& [name, strategy] : strategies) {
            for (size_t shards : {4, 8, 16, 32}) {
                auto result = run_insert_benchmark(8, 100000, shards, strategy);
                std::cout << std::setw(12) << name
                          << std::setw(10) << shards
                          << std::setw(12) << std::fixed << std::setprecision(0) 
                          << result.throughput_ops_sec << " ops/s"
                          << std::setw(10) << std::setprecision(1) 
                          << (result.balance_score * 100) << "%"
                          << std::setw(12) << result.total_elements << "\n";
            }
            std::cout << "\n";
        }

        // Test 3: Mixed Workload
        std::cout << "\n=== Test 3: Mixed Workload (80% reads, 20% writes) ===\n\n";
        std::cout << std::setw(12) << "Strategy" 
                  << std::setw(15) << "Throughput"
                  << std::setw(12) << "Balance"
                  << std::setw(12) << "Latency" << "\n";
        std::cout << std::string(51, '-') << "\n";

        for (const auto& [name, strategy] : strategies) {
            auto result = run_mixed_benchmark(8, 200000, 8, strategy, 0.8);
            std::cout << std::setw(12) << name
                      << std::setw(12) << std::fixed << std::setprecision(0) 
                      << result.throughput_ops_sec << " ops/s"
                      << std::setw(10) << std::setprecision(1) 
                      << (result.balance_score * 100) << "%"
                      << std::setw(10) << std::setprecision(2) 
                      << result.latency_us << " us" << "\n";
        }

        std::cout << "\n================================================================================\n";
        std::cout << "                              BENCHMARK COMPLETE\n";
        std::cout << "================================================================================\n";
    }
};

int main() {
    HeavyBenchmark bench;
    bench.run_all();
    return 0;
}
