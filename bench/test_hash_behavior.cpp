// Test to verify hash behavior between compilers
#include <iostream>
#include <functional>
#include <map>

int main() {
    std::cout << "=== Hash Behavior Test ===\n\n";
    
    // The attack pattern: (key/8)*8 forces collisions
    std::map<size_t, int> shard_counts;
    constexpr size_t NUM_SHARDS = 8;
    
    std::cout << "Attack pattern: key = (i/8)*8 for i in [0, 40000)\n";
    std::cout << "This should produce only 5000 unique keys\n\n";
    
    int unique_keys = 0;
    for (int i = 0; i < 40000; ++i) {
        int key = (i / 8) * 8;
        size_t shard = std::hash<int>{}(key) % NUM_SHARDS;
        
        if (i < 40000 / 8) {  // Only count unique keys
            shard_counts[shard]++;
            unique_keys++;
        }
    }
    
    std::cout << "Unique keys: " << unique_keys << "\n";
    std::cout << "Shard distribution of unique keys:\n";
    for (size_t s = 0; s < NUM_SHARDS; ++s) {
        std::cout << "  Shard " << s << ": " << shard_counts[s] << "\n";
    }
    
    // Show first 10 hashes
    std::cout << "\nFirst 10 key hashes:\n";
    for (int i = 0; i < 10; ++i) {
        int key = i * 8;  // 0, 8, 16, 24, ...
        size_t hash = std::hash<int>{}(key);
        size_t shard = hash % NUM_SHARDS;
        std::cout << "  key=" << key << " -> hash=" << hash << " -> shard=" << shard << "\n";
    }
    
    return 0;
}
