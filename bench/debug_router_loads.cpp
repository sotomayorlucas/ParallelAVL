#include "../include/parallel_avl.hpp"
#include "../include/router.hpp"
#include <iostream>
#include <thread>
#include <vector>

int main() {
    std::cout << "=== Debug Router Internal Loads ===\n\n";

    // Create router directly to inspect its internal state
    AdversaryResistantRouter<int> router(8, AdversaryResistantRouter<int>::Strategy::LOAD_AWARE);
    
    // Simulate the attack pattern on router only
    constexpr size_t NUM_THREADS = 8;
    constexpr size_t OPS_PER_THREAD = 5000;
    
    std::vector<std::thread> threads;
    for (size_t tid = 0; tid < NUM_THREADS; ++tid) {
        threads.emplace_back([&router, tid]() {
            for (size_t i = 0; i < OPS_PER_THREAD; ++i) {
                int key = tid * OPS_PER_THREAD + i;
                key = (key / 8) * 8;
                
                size_t shard = router.route(key);
                router.record_insertion(shard);
            }
        });
    }
    for (auto& t : threads) t.join();
    
    auto stats = router.get_stats();
    
    std::cout << "Router internal state:\n";
    std::cout << "  Total load (sum of shard_loads_): " << stats.total_load << "\n";
    std::cout << "  Min load: " << stats.min_load << "\n";
    std::cout << "  Max load: " << stats.max_load << "\n";
    std::cout << "  Avg load: " << stats.avg_load << "\n";
    std::cout << "  Balance score: " << (stats.balance_score * 100) << "%\n";
    std::cout << "  Has hotspot: " << (stats.has_hotspot ? "YES" : "No") << "\n";
    std::cout << "  Suspicious patterns: " << stats.suspicious_patterns << "\n";
    std::cout << "  Blocked redirects: " << stats.blocked_redirects << "\n";
    
    std::cout << "\nExpected:\n";
    std::cout << "  If each of 40000 ops counted, total_load = 40000\n";
    std::cout << "  If balance is 0%, it means all load went to one shard\n";
    
    // Calculate what the distribution should be
    std::cout << "\nThis confirms: router counts ALL insert attempts,\n";
    std::cout << "not just successful unique insertions.\n";
    std::cout << "With (key/8)*8, all keys hash to SAME values,\n";
    std::cout << "so shard_loads_[0] gets most of the count.\n";
    
    return 0;
}
