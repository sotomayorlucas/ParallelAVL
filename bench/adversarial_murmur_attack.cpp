/**
 * Adversarial Attack against Murmur3 Hash
 * 
 * Intenta romper el hash robusto buscando colisiones
 */

#include "../include/parallel_avl.hpp"
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <random>
#include <iomanip>
#include <map>
#include <set>

// Replicar el hash robusto para análisis
size_t murmur_finalizer(size_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

size_t robust_hash(int key) {
    return murmur_finalizer(std::hash<int>{}(key));
}

class MurmurAttacker {
public:
    // Ataque 1: Buscar keys que colisionen en el mismo shard
    static std::vector<int> find_colliding_keys(size_t target_shard, size_t num_shards, size_t count) {
        std::vector<int> keys;
        for (int k = 0; keys.size() < count && k < 100000000; ++k) {
            if (robust_hash(k) % num_shards == target_shard) {
                keys.push_back(k);
            }
        }
        return keys;
    }

    // Ataque 2: Birthday attack - buscar muchas keys para un shard
    static void birthday_attack(
        ParallelAVL<int, int>& tree,
        size_t target_shard,
        size_t num_shards,
        size_t num_keys
    ) {
        auto keys = find_colliding_keys(target_shard, num_shards, num_keys);
        for (int k : keys) {
            tree.insert(k, k);
        }
    }

    // Ataque 3: Reverse engineering attempt - buscar patrones
    static void pattern_attack(ParallelAVL<int, int>& tree, size_t num_keys) {
        // Intentar diferentes patrones matemáticos
        for (size_t i = 0; i < num_keys; ++i) {
            // Patrón 1: Potencias de 2
            tree.insert(1 << (i % 30), i);
            // Patrón 2: Fibonacci-like
            tree.insert(static_cast<int>(i * 1618033988), i);
            // Patrón 3: Primos
            tree.insert(static_cast<int>(i * 104729), i);
        }
    }
};

void run_attack(const std::string& name, auto attack_fn, size_t num_shards, 
                ParallelAVL<int, int>::RouterStrategy strategy) {
    ParallelAVL<int, int> tree(num_shards, strategy);
    
    auto start = std::chrono::high_resolution_clock::now();
    attack_fn(tree);
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    auto stats = tree.get_stats();
    
    std::cout << "[" << name << "]\n";
    std::cout << "  Time:        " << duration << " ms\n";
    std::cout << "  Balance:     " << std::fixed << std::setprecision(1) 
              << (stats.balance_score * 100) << "%\n";
    std::cout << "  Hotspot:     " << (stats.has_hotspot ? "YES" : "No") << "\n";
    std::cout << "  Total size:  " << stats.total_size << "\n";
    std::cout << "  Distribution: [";
    for (size_t i = 0; i < stats.shard_sizes.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << stats.shard_sizes[i];
    }
    std::cout << "]\n";
    
    // Calcular CV (Coefficient of Variation)
    double mean = stats.total_size / static_cast<double>(num_shards);
    double variance = 0;
    for (auto s : stats.shard_sizes) {
        double diff = s - mean;
        variance += diff * diff;
    }
    double std_dev = std::sqrt(variance / num_shards);
    double cv = mean > 0 ? std_dev / mean : 0;
    std::cout << "  CV:          " << std::setprecision(3) << cv << "\n\n";
}

int main() {
    std::cout << "\n";
    std::cout << "================================================================================\n";
    std::cout << "           ADVERSARIAL ATTACKS AGAINST MURMUR3 HASH\n";
    std::cout << "================================================================================\n\n";

    constexpr size_t NUM_SHARDS = 8;
    constexpr size_t ATTACK_KEYS = 5000;

    std::vector<std::pair<std::string, ParallelAVL<int, int>::RouterStrategy>> strategies = {
        {"Static Hash", ParallelAVL<int, int>::RouterStrategy::STATIC_HASH},
        {"Load-Aware", ParallelAVL<int, int>::RouterStrategy::LOAD_AWARE},
        {"Intelligent", ParallelAVL<int, int>::RouterStrategy::INTELLIGENT},
    };

    // Ataque 1: Targeted Collision (fuerza bruta para encontrar colisiones)
    std::cout << "=== Attack 1: Targeted Collision (brute force) ===\n";
    std::cout << "Finding " << ATTACK_KEYS << " keys that hash to shard 0...\n\n";
    
    for (const auto& [name, strategy] : strategies) {
        run_attack(name, [&](ParallelAVL<int, int>& tree) {
            MurmurAttacker::birthday_attack(tree, 0, NUM_SHARDS, ATTACK_KEYS);
        }, NUM_SHARDS, strategy);
    }

    // Ataque 2: Multi-shard collision
    std::cout << "=== Attack 2: Multi-Thread Collision Attack ===\n";
    std::cout << "8 threads each targeting different shards...\n\n";
    
    for (const auto& [name, strategy] : strategies) {
        run_attack(name, [&](ParallelAVL<int, int>& tree) {
            std::vector<std::thread> threads;
            for (size_t t = 0; t < 8; ++t) {
                threads.emplace_back([&tree, t]() {
                    auto keys = MurmurAttacker::find_colliding_keys(t % 8, 8, 625);
                    for (int k : keys) {
                        tree.insert(k, k);
                    }
                });
            }
            for (auto& th : threads) th.join();
        }, NUM_SHARDS, strategy);
    }

    // Ataque 3: Pattern-based attack
    std::cout << "=== Attack 3: Mathematical Pattern Attack ===\n";
    std::cout << "Using powers of 2, Fibonacci, primes...\n\n";
    
    for (const auto& [name, strategy] : strategies) {
        run_attack(name, [&](ParallelAVL<int, int>& tree) {
            MurmurAttacker::pattern_attack(tree, ATTACK_KEYS / 3);
        }, NUM_SHARDS, strategy);
    }

    // Ataque 4: Random baseline (para comparación)
    std::cout << "=== Baseline: Random Keys (no attack) ===\n\n";
    
    for (const auto& [name, strategy] : strategies) {
        run_attack(name, [&](ParallelAVL<int, int>& tree) {
            std::mt19937 rng(42);
            std::uniform_int_distribution<int> dist(0, 10000000);
            for (size_t i = 0; i < ATTACK_KEYS; ++i) {
                tree.insert(dist(rng), i);
            }
        }, NUM_SHARDS, strategy);
    }

    std::cout << "================================================================================\n";
    std::cout << "                              ATTACK COMPLETE\n";
    std::cout << "================================================================================\n";

    return 0;
}
