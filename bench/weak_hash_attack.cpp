/**
 * Weak Hash Attack Benchmark
 * 
 * Usa hash débil (identity) para probar balanceo bajo ataques
 * Permite comparar GCC vs ICX de manera justa
 */

#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include <iomanip>
#include <mutex>
#include <map>
#include <cmath>
#include <functional>

// Implementación simplificada con hash configurable
template<typename Key, typename Value>
class TestableParallelAVL {
public:
    enum class HashType { WEAK, ROBUST };
    enum class Strategy { STATIC_HASH, LOAD_AWARE, INTELLIGENT };

private:
    struct Shard {
        std::mutex mtx;
        std::map<Key, Value> data;
        std::atomic<size_t> size{0};
    };

    std::vector<Shard> shards_;
    size_t num_shards_;
    HashType hash_type_;
    Strategy strategy_;
    std::vector<std::atomic<size_t>> shard_loads_;
    
    static constexpr double HOTSPOT_THRESHOLD = 1.5;

    // Hash débil - vulnerable a ataques triviales
    size_t weak_hash(const Key& key) const {
        return static_cast<size_t>(key);  // Identity hash
    }

    // Hash robusto - Murmur3 finalizer
    size_t robust_hash(const Key& key) const {
        size_t h = static_cast<size_t>(key);
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ULL;
        h ^= h >> 33;
        return h;
    }

    size_t compute_hash(const Key& key) const {
        return (hash_type_ == HashType::WEAK) ? weak_hash(key) : robust_hash(key);
    }

    size_t route(const Key& key) {
        size_t natural = compute_hash(key) % num_shards_;
        
        if (strategy_ == Strategy::STATIC_HASH) {
            return natural;
        }
        
        // Load-Aware o Intelligent: buscar shard menos cargado si hay hotspot
        size_t my_load = shard_loads_[natural].load(std::memory_order_relaxed);
        size_t total = 0;
        for (size_t i = 0; i < num_shards_; ++i) {
            total += shard_loads_[i].load(std::memory_order_relaxed);
        }
        double avg = total / static_cast<double>(num_shards_);
        
        if (my_load > HOTSPOT_THRESHOLD * avg && avg > 0) {
            // Hotspot detectado - redirigir al shard menos cargado
            size_t min_load = SIZE_MAX;
            size_t min_shard = natural;
            for (size_t i = 0; i < num_shards_; ++i) {
                size_t load = shard_loads_[i].load(std::memory_order_relaxed);
                if (load < min_load) {
                    min_load = load;
                    min_shard = i;
                }
            }
            return min_shard;
        }
        
        return natural;
    }

public:
    TestableParallelAVL(size_t num_shards, HashType hash_type, Strategy strategy)
        : shards_(num_shards)
        , num_shards_(num_shards)
        , hash_type_(hash_type)
        , strategy_(strategy)
        , shard_loads_(num_shards)
    {
        for (auto& l : shard_loads_) l = 0;
    }

    void insert(const Key& key, const Value& value) {
        size_t shard = route(key);
        
        std::lock_guard lock(shards_[shard].mtx);
        auto [it, inserted] = shards_[shard].data.insert_or_assign(key, value);
        if (inserted) {
            shards_[shard].size.fetch_add(1, std::memory_order_relaxed);
            shard_loads_[shard].fetch_add(1, std::memory_order_relaxed);
        }
    }

    struct Stats {
        size_t total_size = 0;
        std::vector<size_t> shard_sizes;
        double balance_score = 0;
        bool has_hotspot = false;
    };

    Stats get_stats() const {
        Stats s;
        s.shard_sizes.resize(num_shards_);
        
        size_t max_size = 0, min_size = SIZE_MAX;
        for (size_t i = 0; i < num_shards_; ++i) {
            s.shard_sizes[i] = shards_[i].size.load(std::memory_order_relaxed);
            s.total_size += s.shard_sizes[i];
            max_size = std::max(max_size, s.shard_sizes[i]);
            min_size = std::min(min_size, s.shard_sizes[i]);
        }
        
        double avg = s.total_size / static_cast<double>(num_shards_);
        if (avg > 0) {
            double variance = 0;
            for (auto sz : s.shard_sizes) {
                double diff = sz - avg;
                variance += diff * diff;
            }
            double std_dev = std::sqrt(variance / num_shards_);
            s.balance_score = std::max(0.0, 1.0 - (std_dev / avg));
            s.has_hotspot = (max_size > HOTSPOT_THRESHOLD * avg);
        } else {
            s.balance_score = 1.0;
        }
        
        return s;
    }
};

void run_test(
    const std::string& name,
    TestableParallelAVL<int, int>::HashType hash_type,
    TestableParallelAVL<int, int>::Strategy strategy,
    size_t num_threads,
    size_t ops_per_thread,
    bool targeted_attack
) {
    using Tree = TestableParallelAVL<int, int>;
    Tree tree(8, hash_type, strategy);
    
    auto start = std::chrono::high_resolution_clock::now();
    
    std::vector<std::thread> threads;
    for (size_t t = 0; t < num_threads; ++t) {
        threads.emplace_back([&tree, t, ops_per_thread, targeted_attack]() {
            for (size_t i = 0; i < ops_per_thread; ++i) {
                int key;
                if (targeted_attack) {
                    // Ataque: keys múltiplos de 8 → todos van al shard 0 con weak hash
                    key = static_cast<int>((t * ops_per_thread + i) / 8) * 8;
                } else {
                    // Normal: keys secuenciales
                    key = static_cast<int>(t * ops_per_thread + i);
                }
                tree.insert(key, key);
            }
        });
    }
    for (auto& th : threads) th.join();
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    auto stats = tree.get_stats();
    
    std::cout << "[" << name << "]\n";
    std::cout << "  Time:        " << duration << " ms\n";
    std::cout << "  Balance:     " << std::fixed << std::setprecision(1) 
              << (stats.balance_score * 100) << "%\n";
    std::cout << "  Hotspot:     " << (stats.has_hotspot ? "YES" : "No") << "\n";
    std::cout << "  Total:       " << stats.total_size << "\n";
    std::cout << "  Distribution: [";
    for (size_t i = 0; i < stats.shard_sizes.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << stats.shard_sizes[i];
    }
    std::cout << "]\n\n";
}

int main() {
    using Tree = TestableParallelAVL<int, int>;
    
    std::cout << "\n";
    std::cout << "================================================================================\n";
    std::cout << "            WEAK HASH ATTACK - BALANCE TEST (GCC vs ICX)\n";
    std::cout << "================================================================================\n";
    std::cout << "\nCompiler: "
#if defined(__INTEL_COMPILER) || defined(__INTEL_LLVM_COMPILER)
              << "ICX (Intel)\n";
#elif defined(__GNUC__)
              << "GCC " << __GNUC__ << "." << __GNUC_MINOR__ << "\n";
#else
              << "Unknown\n";
#endif

    constexpr size_t THREADS = 8;
    constexpr size_t OPS = 5000;

    std::cout << "\n=== Test 1: Normal Traffic (no attack) ===\n\n";
    
    std::cout << "--- WEAK HASH ---\n";
    run_test("Static Hash", Tree::HashType::WEAK, Tree::Strategy::STATIC_HASH, THREADS, OPS, false);
    run_test("Load-Aware", Tree::HashType::WEAK, Tree::Strategy::LOAD_AWARE, THREADS, OPS, false);
    
    std::cout << "--- ROBUST HASH ---\n";
    run_test("Static Hash", Tree::HashType::ROBUST, Tree::Strategy::STATIC_HASH, THREADS, OPS, false);
    run_test("Load-Aware", Tree::HashType::ROBUST, Tree::Strategy::LOAD_AWARE, THREADS, OPS, false);

    std::cout << "\n=== Test 2: Targeted Attack (keys = multiples of 8) ===\n\n";
    
    std::cout << "--- WEAK HASH (vulnerable) ---\n";
    run_test("Static Hash", Tree::HashType::WEAK, Tree::Strategy::STATIC_HASH, THREADS, OPS, true);
    run_test("Load-Aware", Tree::HashType::WEAK, Tree::Strategy::LOAD_AWARE, THREADS, OPS, true);
    
    std::cout << "--- ROBUST HASH (protected) ---\n";
    run_test("Static Hash", Tree::HashType::ROBUST, Tree::Strategy::STATIC_HASH, THREADS, OPS, true);
    run_test("Load-Aware", Tree::HashType::ROBUST, Tree::Strategy::LOAD_AWARE, THREADS, OPS, true);

    std::cout << "================================================================================\n";
    std::cout << "                              TEST COMPLETE\n";
    std::cout << "================================================================================\n";

    return 0;
}
