// =============================================================================
// Benchmark: Dynamic Sharded Tree - Scaling con Migración Activa
// =============================================================================

#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <vector>
#include <random>
#include <atomic>

#include "../include/DynamicShardedTree.hpp"
#include "../include/AVLTreeParallelV2.h"

using namespace std::chrono;

// -----------------------------------------------------------------------------
// Utilidades
// -----------------------------------------------------------------------------

template<typename Func>
double measure_ms(Func&& f) {
    auto start = high_resolution_clock::now();
    f();
    auto end = high_resolution_clock::now();
    return duration_cast<microseconds>(end - start).count() / 1000.0;
}

void print_header(const std::string& title) {
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║  " << std::left << std::setw(60) << title << "║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
}

// -----------------------------------------------------------------------------
// Benchmark 1: Balance durante scaling (comparación V1 vs Dynamic)
// -----------------------------------------------------------------------------

void benchmark_scaling_balance() {
    print_header("Benchmark 1: Balance Score durante Scaling");
    
    const size_t OPS_PER_PHASE = 50000;
    std::mt19937 rng(42);
    std::uniform_int_distribution<> key_dist(0, 999999);
    
    std::cout << "\n  Comparando AVLTreeParallelV2 (sin migración) vs DynamicShardedTree\n\n";
    
    // --- AVLTreeParallelV2 (baseline - sin migración) ---
    std::cout << "  === AVLTreeParallelV2 (baseline) ===\n";
    {
        AVLTreeParallelV2<int, int> tree(4);
        tree.set_prediction_enabled(false);
        
        // Fase 1: 4 shards
        rng.seed(42);
        for (size_t i = 0; i < OPS_PER_PHASE; ++i) {
            tree.insert(key_dist(rng), static_cast<int>(i));
        }
        auto info1 = tree.getArchitectureInfo();
        std::cout << "    Phase 1 (4 shards): " << info1.total_elements 
                  << " elements, Balance: " << std::fixed << std::setprecision(1)
                  << (info1.load_balance_score * 100) << "%\n";
        
        // Fase 2: agregar shards (sin migración)
        tree.add_shard();
        tree.add_shard();
        
        for (size_t i = 0; i < OPS_PER_PHASE; ++i) {
            tree.insert(key_dist(rng), static_cast<int>(i));
        }
        auto info2 = tree.getArchitectureInfo();
        std::cout << "    Phase 2 (6 shards): " << info2.total_elements 
                  << " elements, Balance: " << std::setprecision(1)
                  << (info2.load_balance_score * 100) << "%\n";
        
        // Fase 3: más shards
        tree.add_shard();
        tree.add_shard();
        
        for (size_t i = 0; i < OPS_PER_PHASE; ++i) {
            tree.insert(key_dist(rng), static_cast<int>(i));
        }
        auto info3 = tree.getArchitectureInfo();
        std::cout << "    Phase 3 (8 shards): " << info3.total_elements 
                  << " elements, Balance: " << std::setprecision(1)
                  << (info3.load_balance_score * 100) << "% ⚠️\n";
    }
    
    // --- DynamicShardedTree (con migración) ---
    std::cout << "\n  === DynamicShardedTree (con migración) ===\n";
    {
        DynamicShardedTree<int, int>::Config config;
        config.initial_shards = 4;
        config.vnodes_per_shard = 64;
        
        DynamicShardedTree<int, int> tree(config);
        
        // Fase 1: 4 shards
        rng.seed(42);
        for (size_t i = 0; i < OPS_PER_PHASE; ++i) {
            tree.insert(key_dist(rng), static_cast<int>(i));
        }
        auto stats1 = tree.get_stats();
        std::cout << "    Phase 1 (4 shards): " << stats1.total_elements 
                  << " elements, Balance: " << std::setprecision(1)
                  << (stats1.balance_score * 100) << "%\n";
        
        // Fase 2: agregar shards + migrar
        tree.add_shard();
        tree.add_shard();
        
        for (size_t i = 0; i < OPS_PER_PHASE; ++i) {
            tree.insert(key_dist(rng), static_cast<int>(i));
        }
        
        // Forzar migración completa para medir balance real
        tree.force_rebalance();
        
        auto stats2 = tree.get_stats();
        std::cout << "    Phase 2 (6 shards): " << stats2.total_elements 
                  << " elements, Balance: " << std::setprecision(1)
                  << (stats2.balance_score * 100) << "%\n";
        
        // Fase 3: más shards + migrar
        tree.add_shard();
        tree.add_shard();
        
        for (size_t i = 0; i < OPS_PER_PHASE; ++i) {
            tree.insert(key_dist(rng), static_cast<int>(i));
        }
        
        tree.force_rebalance();
        
        auto stats3 = tree.get_stats();
        std::cout << "    Phase 3 (8 shards): " << stats3.total_elements 
                  << " elements, Balance: " << std::setprecision(1)
                  << (stats3.balance_score * 100) << "% ✓\n";
    }
}

// -----------------------------------------------------------------------------
// Benchmark 2: Overhead de rebalance
// -----------------------------------------------------------------------------

void benchmark_migration_overhead() {
    print_header("Benchmark 2: Overhead de Rebalance");
    
    const size_t NUM_ELEMENTS = 100000;
    std::mt19937 rng(42);
    std::uniform_int_distribution<> key_dist(0, 999999);
    
    std::cout << "\n  Midiendo tiempo de agregar shard + rebalance con " << NUM_ELEMENTS << " elementos\n\n";
    
    DynamicShardedTree<int, int>::Config config;
    config.initial_shards = 4;
    config.vnodes_per_shard = 64;
    
    DynamicShardedTree<int, int> tree(config);
    
    // Insertar datos iniciales
    for (size_t i = 0; i < NUM_ELEMENTS; ++i) {
        tree.insert(key_dist(rng), static_cast<int>(i));
    }
    
    auto stats_before = tree.get_stats();
    std::cout << "  Antes: " << stats_before.num_shards << " shards, "
              << stats_before.total_elements << " elements, Balance: "
              << std::setprecision(1) << (stats_before.balance_score * 100) << "%\n";
    
    // Medir tiempo de add_shard
    double add_time = measure_ms([&] {
        tree.add_shard();
    });
    
    auto stats_after_add = tree.get_stats();
    std::cout << "  Después de add_shard: " << stats_after_add.num_shards << " shards\n";
    std::cout << "    add_shard() time: " << std::setprecision(2) << add_time << " ms\n";
    
    // Medir tiempo de rebalance completo
    double rebalance_time = measure_ms([&] {
        tree.force_rebalance();
    });
    
    auto stats_after = tree.get_stats();
    std::cout << "  Después de rebalance: Balance: "
              << std::setprecision(1) << (stats_after.balance_score * 100) << "%\n";
    std::cout << "    force_rebalance() time: " << std::setprecision(2) << rebalance_time << " ms\n";
    
    // Calcular throughput
    double keys_per_sec = stats_after.total_elements / (rebalance_time / 1000.0);
    std::cout << "    Rebalance throughput: " << std::setprecision(0) << keys_per_sec << " keys/sec\n";
}

// -----------------------------------------------------------------------------
// Benchmark 3: Consistent Hash - distribución uniforme
// -----------------------------------------------------------------------------

void benchmark_consistent_hash() {
    print_header("Benchmark 3: Consistent Hash - Distribución Uniforme");
    
    const size_t NUM_ELEMENTS = 50000;
    std::mt19937 rng(42);
    std::uniform_int_distribution<> key_dist(0, 999999);
    
    std::cout << "\n  Verificando distribución uniforme con consistent hashing\n\n";
    
    DynamicShardedTree<int, int>::Config config;
    config.initial_shards = 4;
    config.vnodes_per_shard = 64;
    
    DynamicShardedTree<int, int> tree(config);
    
    // Insertar datos
    for (size_t i = 0; i < NUM_ELEMENTS; ++i) {
        tree.insert(key_dist(rng), static_cast<int>(i));
    }
    
    auto stats_before = tree.get_stats();
    std::cout << "  Antes (4 shards):\n";
    std::cout << "    Balance: " << std::setprecision(1) << (stats_before.balance_score * 100) << "%\n";
    for (size_t i = 0; i < stats_before.elements_per_shard.size(); ++i) {
        std::cout << "    Shard " << i << ": " << stats_before.elements_per_shard[i] << "\n";
    }
    
    // Agregar shard y rebalancear
    tree.add_shard();
    tree.force_rebalance();
    
    auto stats_after = tree.get_stats();
    std::cout << "\n  Después (5 shards + rebalance):\n";
    std::cout << "    Balance: " << std::setprecision(1) << (stats_after.balance_score * 100) << "%\n";
    for (size_t i = 0; i < stats_after.elements_per_shard.size(); ++i) {
        std::cout << "    Shard " << i << ": " << stats_after.elements_per_shard[i] << "\n";
    }
    
    bool good_balance = stats_after.balance_score > 0.9;
    std::cout << "\n  Good balance after scaling: " << (good_balance ? "YES ✓" : "NO ⚠️") << "\n";
}

// -----------------------------------------------------------------------------
// Benchmark 4: Operaciones concurrentes con scaling
// -----------------------------------------------------------------------------

void benchmark_operations_during_migration() {
    print_header("Benchmark 4: Operaciones Concurrentes con Scaling");
        
    const size_t NUM_ELEMENTS = 50000;
    const size_t OPS_AFTER_SCALE = 10000;
        
    std::cout << "\n  Verificando operaciones después de escalar (migración lazy)\n\n";
        
    DynamicShardedTree<int, int>::Config config;
    config.initial_shards = 4;
    config.vnodes_per_shard = 64;
        
    DynamicShardedTree<int, int> tree(config);
        
    std::mt19937 rng(42);
    std::uniform_int_distribution<> key_dist(0, 999999);
        
    // Insertar datos iniciales
    std::vector<int> inserted_keys;
    for (size_t i = 0; i < NUM_ELEMENTS; ++i) {
        int key = key_dist(rng);
        tree.insert(key, key * 10);
        inserted_keys.push_back(key);
    }
        
    std::cout << "  Initial: " << tree.size() << " elements, 4 shards\n";
        
    // Escalar SIN rebalance (lazy migration)
    tree.add_shard();
    tree.add_shard();
        
    std::cout << "  After scaling: 6 shards (sin rebalance explícito)\n";
        
    // Hacer operaciones - la migración lazy ocurre durante contains/get
    std::atomic<size_t> successful_reads{0};
    std::atomic<size_t> successful_writes{0};
        
    double ops_time = measure_ms([&] {
        std::shuffle(inserted_keys.begin(), inserted_keys.end(), rng);
            
        for (size_t i = 0; i < OPS_AFTER_SCALE; ++i) {
            int key = inserted_keys[i % inserted_keys.size()];
                
            if (i % 3 == 0) {
                tree.contains(key);
                successful_reads++;
            } else if (i % 3 == 1) {
                int new_key = key_dist(rng);
                tree.insert(new_key, new_key * 10);
                successful_writes++;
            } else {
                tree.get(key);
                successful_reads++;
            }
        }
    });
        
    auto stats_after_ops = tree.get_stats();
        
    std::cout << "  After " << OPS_AFTER_SCALE << " operations:\n";
    std::cout << "    Reads: " << successful_reads << ", Writes: " << successful_writes << "\n";
    std::cout << "    Time: " << std::setprecision(2) << ops_time << " ms\n";
    std::cout << "    Elements: " << stats_after_ops.total_elements << "\n";
    std::cout << "    Balance (lazy): " << std::setprecision(1) 
              << (stats_after_ops.balance_score * 100) << "%\n";
        
    // Ahora forzar rebalance completo
    tree.force_rebalance();
    auto stats_final = tree.get_stats();
    std::cout << "    Balance (after rebalance): " 
              << (stats_final.balance_score * 100) << "% ✓\n";
}

// -----------------------------------------------------------------------------
// Benchmark 5: Throughput comparison
// -----------------------------------------------------------------------------

void benchmark_throughput() {
    print_header("Benchmark 5: Throughput - Dynamic vs Static");
    
    const size_t NUM_THREADS = std::thread::hardware_concurrency();
    const size_t OPS_PER_THREAD = 50000;
    
    std::cout << "\n  " << NUM_THREADS << " threads, " << OPS_PER_THREAD << " ops/thread\n\n";
    
    // --- Static (AVLTreeParallelV2) ---
    double static_time;
    {
        AVLTreeParallelV2<int, int> tree(8);
        tree.set_prediction_enabled(false);
        
        std::atomic<size_t> ops{0};
        
        static_time = measure_ms([&] {
            std::vector<std::thread> threads;
            for (size_t t = 0; t < NUM_THREADS; ++t) {
                threads.emplace_back([&, t] {
                    std::mt19937 rng(t);
                    std::uniform_int_distribution<> dist(0, 999999);
                    
                    for (size_t i = 0; i < OPS_PER_THREAD; ++i) {
                        int key = dist(rng);
                        if (i % 3 == 0) {
                            tree.contains(key);
                        } else {
                            tree.insert(key, key);
                        }
                        ops++;
                    }
                });
            }
            for (auto& t : threads) t.join();
        });
        
        std::cout << "  Static (AVLTreeParallelV2):  " << std::setprecision(2) << static_time << " ms, "
                  << std::setprecision(0) << (ops / (static_time / 1000.0)) << " ops/s\n";
    }
    
    // --- Dynamic (DynamicShardedTree) ---
    double dynamic_time;
    {
        DynamicShardedTree<int, int>::Config config;
        config.initial_shards = 8;
        
        DynamicShardedTree<int, int> tree(config);
        
        std::atomic<size_t> ops{0};
        
        dynamic_time = measure_ms([&] {
            std::vector<std::thread> threads;
            for (size_t t = 0; t < NUM_THREADS; ++t) {
                threads.emplace_back([&, t] {
                    std::mt19937 rng(t);
                    std::uniform_int_distribution<> dist(0, 999999);
                    
                    for (size_t i = 0; i < OPS_PER_THREAD; ++i) {
                        int key = dist(rng);
                        if (i % 3 == 0) {
                            tree.contains(key);
                        } else {
                            tree.insert(key, key);
                        }
                        ops++;
                    }
                });
            }
            for (auto& t : threads) t.join();
        });
        
        std::cout << "  Dynamic (DynamicShardedTree): " << std::setprecision(2) << dynamic_time << " ms, "
                  << std::setprecision(0) << (ops / (dynamic_time / 1000.0)) << " ops/s\n";
    }
    
    double overhead = ((dynamic_time / static_time) - 1) * 100;
    std::cout << "\n  Overhead of consistent hashing: " << std::setprecision(1) 
              << (overhead > 0 ? "+" : "") << overhead << "%\n";
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

int main() {
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║       DYNAMIC SHARDED TREE - COMPREHENSIVE BENCHMARK         ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
    
    benchmark_scaling_balance();
    benchmark_migration_overhead();
    benchmark_consistent_hash();
    benchmark_operations_during_migration();
    benchmark_throughput();
    
    std::cout << "\n╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                    BENCHMARK COMPLETE                        ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n\n";
    
    return 0;
}
