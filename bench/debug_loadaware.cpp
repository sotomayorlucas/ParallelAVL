#include "../include/parallel_avl.hpp"
#include <iostream>
#include <thread>
#include <vector>
#include <iomanip>
#include <cmath>

int main() {
    std::cout << "=== Debug Load-Aware Balance ===\n\n";

    constexpr size_t NUM_THREADS = 8;
    constexpr size_t OPS_PER_THREAD = 5000;

    // Simulating Targeted Shard Attack with LOAD_AWARE
    {
        std::cout << "Test: Targeted Shard Attack with LOAD_AWARE\n";
        ParallelAVL<int, int> tree(8, ParallelAVL<int, int>::RouterStrategy::LOAD_AWARE);
        
        std::vector<std::thread> threads;
        for (size_t tid = 0; tid < NUM_THREADS; ++tid) {
            threads.emplace_back([&tree, tid]() {
                for (size_t i = 0; i < OPS_PER_THREAD; ++i) {
                    int key = tid * OPS_PER_THREAD + i;
                    key = (key / 8) * 8;  // Force collisions
                    tree.insert(key, key);
                }
            });
        }
        for (auto& t : threads) t.join();
        
        auto stats = tree.get_stats();
        
        // Calculate expected balance from shard_sizes
        double total = 0;
        for (auto s : stats.shard_sizes) total += s;
        double mean = total / stats.shard_sizes.size();
        
        double variance = 0;
        for (auto s : stats.shard_sizes) {
            double diff = s - mean;
            variance += diff * diff;
        }
        variance /= stats.shard_sizes.size();
        double std_dev = std::sqrt(variance);
        double expected_balance = (mean > 0) ? std::max(0.0, 1.0 - (std_dev / mean)) : 1.0;
        
        std::cout << "  Router balance_score: " << std::fixed << std::setprecision(4) 
                  << stats.balance_score << " (" << (stats.balance_score * 100) << "%)\n";
        std::cout << "  Expected from shard_sizes: " << expected_balance 
                  << " (" << (expected_balance * 100) << "%)\n";
        std::cout << "  Total size: " << stats.total_size << "\n";
        std::cout << "  Mean: " << mean << ", StdDev: " << std_dev << "\n";
        std::cout << "  Suspicious patterns: " << stats.suspicious_patterns << "\n";
        std::cout << "  Blocked redirects: " << stats.blocked_redirects << "\n";
        std::cout << "  Redirect index size: " << stats.redirect_index_size << "\n";
        std::cout << "  Distribution: [";
        for (size_t i = 0; i < stats.shard_sizes.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << stats.shard_sizes[i];
        }
        std::cout << "]\n\n";
        
        if (std::abs(stats.balance_score - expected_balance) > 0.01) {
            std::cout << "  *** MISMATCH! Router sees different loads than actual shard sizes ***\n";
            std::cout << "  This indicates router's internal shard_loads_ != actual shard.size()\n";
        }
    }

    // Test with STATIC_HASH for comparison
    {
        std::cout << "\nTest: Same attack with STATIC_HASH (baseline)\n";
        ParallelAVL<int, int> tree(8, ParallelAVL<int, int>::RouterStrategy::STATIC_HASH);
        
        std::vector<std::thread> threads;
        for (size_t tid = 0; tid < NUM_THREADS; ++tid) {
            threads.emplace_back([&tree, tid]() {
                for (size_t i = 0; i < OPS_PER_THREAD; ++i) {
                    int key = tid * OPS_PER_THREAD + i;
                    key = (key / 8) * 8;
                    tree.insert(key, key);
                }
            });
        }
        for (auto& t : threads) t.join();
        
        auto stats = tree.get_stats();
        std::cout << "  Router balance_score: " << std::fixed << std::setprecision(4) 
                  << stats.balance_score << " (" << (stats.balance_score * 100) << "%)\n";
        std::cout << "  Distribution: [";
        for (size_t i = 0; i < stats.shard_sizes.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << stats.shard_sizes[i];
        }
        std::cout << "]\n";
    }

    return 0;
}
