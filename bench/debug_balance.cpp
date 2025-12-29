#include "../include/parallel_avl.hpp"
#include <iostream>
#include <thread>
#include <vector>
#include <iomanip>

int main() {
    std::cout << "=== Debug Balance Score ===\n\n";

    // Test 1: Single-threaded insert
    {
        std::cout << "Test 1: Single-threaded, 1000 sequential keys\n";
        ParallelAVL<int, int> tree(8, ParallelAVL<int, int>::RouterStrategy::STATIC_HASH);
        
        for (int i = 0; i < 1000; ++i) {
            tree.insert(i, i);
        }
        
        auto stats = tree.get_stats();
        std::cout << "  Total size: " << stats.total_size << "\n";
        std::cout << "  Balance score: " << std::fixed << std::setprecision(4) 
                  << stats.balance_score << " (" << (stats.balance_score * 100) << "%)\n";
        std::cout << "  Distribution: [";
        for (size_t i = 0; i < stats.shard_sizes.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << stats.shard_sizes[i];
        }
        std::cout << "]\n\n";
    }

    // Test 2: Multi-threaded
    {
        std::cout << "Test 2: Multi-threaded (4 threads), 1000 keys each\n";
        ParallelAVL<int, int> tree(8, ParallelAVL<int, int>::RouterStrategy::STATIC_HASH);
        
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&tree, t]() {
                for (int i = 0; i < 1000; ++i) {
                    tree.insert(t * 10000 + i, i);
                }
            });
        }
        for (auto& t : threads) t.join();
        
        auto stats = tree.get_stats();
        std::cout << "  Total size: " << stats.total_size << "\n";
        std::cout << "  Balance score: " << std::fixed << std::setprecision(4) 
                  << stats.balance_score << " (" << (stats.balance_score * 100) << "%)\n";
        std::cout << "  Distribution: [";
        for (size_t i = 0; i < stats.shard_sizes.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << stats.shard_sizes[i];
        }
        std::cout << "]\n\n";
    }

    // Test 3: Check router internal loads
    {
        std::cout << "Test 3: Debug - Check if router sees the loads\n";
        ParallelAVL<int, int> tree(8, ParallelAVL<int, int>::RouterStrategy::STATIC_HASH);
        
        // Insert 100 keys
        for (int i = 0; i < 100; ++i) {
            tree.insert(i, i);
        }
        
        auto stats = tree.get_stats();
        
        // Calculate what balance SHOULD be based on shard_sizes
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
        
        std::cout << "  Actual balance_score from router: " << stats.balance_score << "\n";
        std::cout << "  Expected balance based on shard_sizes: " << expected_balance << "\n";
        std::cout << "  Mean: " << mean << ", StdDev: " << std_dev << "\n";
        std::cout << "  Shard sizes: [";
        for (size_t i = 0; i < stats.shard_sizes.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << stats.shard_sizes[i];
        }
        std::cout << "]\n";
        
        if (std::abs(stats.balance_score - expected_balance) > 0.01) {
            std::cout << "\n  *** MISMATCH DETECTED! Router not tracking loads correctly ***\n";
        }
    }

    return 0;
}
