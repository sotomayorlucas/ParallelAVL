// =============================================================================
// Benchmark: Trabajos Futuros - shared_mutex, Predictive Router, Dynamic Shards
// =============================================================================

#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <vector>
#include <random>
#include <atomic>
#include <string>

#include "../include/AVLTreeParallel.h"
#include "../include/AVLTreeParallelV2.h"
#include "../include/PredictiveRouter.hpp"

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

void print_result(const std::string& name, double ms, size_t ops) {
    double ops_per_sec = ops / (ms / 1000.0);
    std::cout << "  " << std::left << std::setw(35) << name 
              << std::right << std::setw(10) << std::fixed << std::setprecision(2) << ms << " ms"
              << std::setw(15) << std::setprecision(0) << ops_per_sec << " ops/s\n";
}

// -----------------------------------------------------------------------------
// Benchmark 1: shared_mutex vs mutex (Read-Heavy Workload)
// -----------------------------------------------------------------------------

void benchmark_shared_mutex() {
    print_header("Benchmark 1: shared_mutex vs mutex (Read-Heavy)");
    
    const size_t NUM_ELEMENTS = 100000;
    const size_t NUM_THREADS = std::thread::hardware_concurrency();
    const size_t OPS_PER_THREAD = 50000;
    const double READ_RATIO = 0.95;  // 95% reads, 5% writes
    
    std::cout << "\n  Config: " << NUM_ELEMENTS << " elements, " 
              << NUM_THREADS << " threads, " << OPS_PER_THREAD << " ops/thread\n";
    std::cout << "  Read ratio: " << (READ_RATIO * 100) << "%\n\n";
    
    // Preparar datos
    std::vector<int> keys(NUM_ELEMENTS);
    for (size_t i = 0; i < NUM_ELEMENTS; ++i) {
        keys[i] = static_cast<int>(i);
    }
    
    // --- V1: mutex tradicional ---
    {
        AVLTreeParallel<int, int> tree_v1(NUM_THREADS);
        
        // Insertar elementos iniciales
        for (int k : keys) {
            tree_v1.insert(k, k * 10);
        }
        
        std::atomic<size_t> total_ops{0};
        
        auto worker = [&](int thread_id) {
            std::mt19937 rng(thread_id);
            std::uniform_real_distribution<> op_dist(0.0, 1.0);
            std::uniform_int_distribution<> key_dist(0, NUM_ELEMENTS - 1);
            
            for (size_t i = 0; i < OPS_PER_THREAD; ++i) {
                int key = key_dist(rng);
                
                if (op_dist(rng) < READ_RATIO) {
                    // Read
                    tree_v1.contains(key);
                } else {
                    // Write
                    tree_v1.insert(key, key * 10);
                }
                total_ops.fetch_add(1, std::memory_order_relaxed);
            }
        };
        
        double ms = measure_ms([&] {
            std::vector<std::thread> threads;
            for (size_t t = 0; t < NUM_THREADS; ++t) {
                threads.emplace_back(worker, t);
            }
            for (auto& t : threads) t.join();
        });
        
        print_result("V1 (std::mutex)", ms, total_ops.load());
    }
    
    // --- V2: shared_mutex ---
    {
        AVLTreeParallelV2<int, int> tree_v2(NUM_THREADS);
        tree_v2.set_prediction_enabled(false);  // Desactivar predicción para comparación justa
        
        // Insertar elementos iniciales
        for (int k : keys) {
            tree_v2.insert(k, k * 10);
        }
        
        std::atomic<size_t> total_ops{0};
        
        auto worker = [&](int thread_id) {
            std::mt19937 rng(thread_id);
            std::uniform_real_distribution<> op_dist(0.0, 1.0);
            std::uniform_int_distribution<> key_dist(0, NUM_ELEMENTS - 1);
            
            for (size_t i = 0; i < OPS_PER_THREAD; ++i) {
                int key = key_dist(rng);
                
                if (op_dist(rng) < READ_RATIO) {
                    // Read (shared lock)
                    tree_v2.contains(key);
                } else {
                    // Write (unique lock)
                    tree_v2.insert(key, key * 10);
                }
                total_ops.fetch_add(1, std::memory_order_relaxed);
            }
        };
        
        double ms = measure_ms([&] {
            std::vector<std::thread> threads;
            for (size_t t = 0; t < NUM_THREADS; ++t) {
                threads.emplace_back(worker, t);
            }
            for (auto& t : threads) t.join();
        });
        
        print_result("V2 (std::shared_mutex)", ms, total_ops.load());
    }
}

// -----------------------------------------------------------------------------
// Benchmark 2: Predictive Router vs Static Hash
// -----------------------------------------------------------------------------

void benchmark_predictive_router() {
    print_header("Benchmark 2: Predictive Router vs Static/Load-Aware");
    
    const size_t NUM_SHARDS = 8;
    const size_t NUM_OPS = 500000;
    
    std::cout << "\n  Config: " << NUM_SHARDS << " shards, " << NUM_OPS << " operations\n";
    std::cout << "  Workload: Zipf distribution (simula hotspots naturales)\n\n";
    
    // Generador Zipf (sesgo hacia keys bajas)
    auto zipf_key = [](std::mt19937& rng, size_t max_key, double skew = 1.5) -> size_t {
        std::uniform_real_distribution<> dist(0.0, 1.0);
        double u = dist(rng);
        return static_cast<size_t>(max_key * std::pow(u, skew));
    };
    
    // --- Static Hash ---
    {
        PredictiveRouter<size_t> router(NUM_SHARDS, PredictiveRouter<size_t>::Strategy::STATIC_HASH);
        std::mt19937 rng(42);
        
        double ms = measure_ms([&] {
            for (size_t i = 0; i < NUM_OPS; ++i) {
                size_t key = zipf_key(rng, 1000000);
                size_t shard = router.route(key);
                router.record_access(shard, i % 10 == 0);
            }
        });
        
        auto stats = router.get_stats();
        print_result("Static Hash", ms, NUM_OPS);
        std::cout << "    Balance: " << std::setprecision(1) << (stats.balance_score * 100) 
                  << "%, Hotspot: " << (stats.has_hotspot ? "YES" : "NO") << "\n";
    }
    
    // --- Load-Aware ---
    {
        PredictiveRouter<size_t> router(NUM_SHARDS, PredictiveRouter<size_t>::Strategy::LOAD_AWARE);
        std::mt19937 rng(42);
        
        double ms = measure_ms([&] {
            for (size_t i = 0; i < NUM_OPS; ++i) {
                size_t key = zipf_key(rng, 1000000);
                size_t shard = router.route(key);
                router.record_access(shard, i % 10 == 0);
            }
        });
        
        auto stats = router.get_stats();
        print_result("Load-Aware", ms, NUM_OPS);
        std::cout << "    Balance: " << std::setprecision(1) << (stats.balance_score * 100) 
                  << "%, Hotspot: " << (stats.has_hotspot ? "YES" : "NO") << "\n";
    }
    
    // --- Predictive ---
    {
        PredictiveRouter<size_t> router(NUM_SHARDS, PredictiveRouter<size_t>::Strategy::PREDICTIVE);
        std::mt19937 rng(42);
        
        double ms = measure_ms([&] {
            for (size_t i = 0; i < NUM_OPS; ++i) {
                size_t key = zipf_key(rng, 1000000);
                size_t shard = router.route(key);
                router.record_access(shard, i % 10 == 0);
            }
        });
        
        auto stats = router.get_stats();
        print_result("Predictive (ML-lite)", ms, NUM_OPS);
        std::cout << "    Balance: " << std::setprecision(1) << (stats.balance_score * 100) 
                  << "%, Hotspot: " << (stats.has_hotspot ? "YES" : "NO")
                  << ", Predictions: " << stats.predictions_made << "\n";
    }
    
    // --- Hybrid ---
    {
        PredictiveRouter<size_t> router(NUM_SHARDS, PredictiveRouter<size_t>::Strategy::HYBRID);
        std::mt19937 rng(42);
        
        double ms = measure_ms([&] {
            for (size_t i = 0; i < NUM_OPS; ++i) {
                size_t key = zipf_key(rng, 1000000);
                size_t shard = router.route(key);
                router.record_access(shard, i % 10 == 0);
            }
        });
        
        auto stats = router.get_stats();
        print_result("Hybrid", ms, NUM_OPS);
        std::cout << "    Balance: " << std::setprecision(1) << (stats.balance_score * 100) 
                  << "%, Hotspot: " << (stats.has_hotspot ? "YES" : "NO") << "\n";
    }
}

// -----------------------------------------------------------------------------
// Benchmark 3: Hotspot Detection Accuracy
// -----------------------------------------------------------------------------

void benchmark_hotspot_prediction() {
    print_header("Benchmark 3: Hotspot Prediction Accuracy");
    
    const size_t NUM_SHARDS = 8;
    const size_t WARMUP_OPS = 10000;
    const size_t TEST_OPS = 5000;
    
    std::cout << "\n  Simulating hotspot emergence...\n\n";
    
    PredictiveRouter<int> router(NUM_SHARDS, PredictiveRouter<int>::Strategy::PREDICTIVE);
    std::mt19937 rng(42);
    
    // Fase 1: Warmup con carga uniforme
    std::uniform_int_distribution<> uniform_dist(0, 99999);
    for (size_t i = 0; i < WARMUP_OPS; ++i) {
        int key = uniform_dist(rng);
        size_t shard = router.route(key);
        router.record_access(shard, false);
    }
    
    auto stats_before = router.get_stats();
    std::cout << "  After warmup (uniform): Balance = " 
              << std::setprecision(1) << (stats_before.balance_score * 100) << "%\n";
    
    // Fase 2: Introducir hotspot (70% de tráfico a shard 0)
    size_t predictions_triggered = 0;
    
    for (size_t i = 0; i < TEST_OPS; ++i) {
        int key;
        if (rng() % 100 < 70) {
            // 70% de keys van al shard 0
            key = static_cast<int>((rng() % 1000) * NUM_SHARDS);  // Keys que hashean a shard 0
        } else {
            key = uniform_dist(rng);
        }
        
        // Verificar predicción ANTES del acceso
        auto pred = router.predict(0);
        if (pred.will_be_hotspot && pred.probability > 0.5) {
            predictions_triggered++;
        }
        
        size_t shard = router.route(key);
        router.record_access(shard, false);
    }
    
    auto stats_after = router.get_stats();
    
    std::cout << "  After hotspot injection: Balance = " 
              << std::setprecision(1) << (stats_after.balance_score * 100) << "%\n";
    std::cout << "  Hotspot detected: " << (stats_after.has_hotspot ? "YES" : "NO") << "\n";
    std::cout << "  Predictions triggered: " << predictions_triggered << " / " << TEST_OPS << "\n";
    
    // Mostrar predicción final para shard 0
    auto pred = router.predict(0);
    std::cout << "\n  Shard 0 prediction:\n";
    std::cout << "    Will be hotspot: " << (pred.will_be_hotspot ? "YES" : "NO") << "\n";
    std::cout << "    Probability: " << std::setprecision(1) << (pred.probability * 100) << "%\n";
    std::cout << "    Predicted load: " << std::setprecision(2) << pred.predicted_load << "\n";
    std::cout << "    Recommended shard: " << pred.recommended_shard << "\n";
}

// -----------------------------------------------------------------------------
// Benchmark 4: Dynamic Shard Scaling
// -----------------------------------------------------------------------------

void benchmark_dynamic_shards() {
    print_header("Benchmark 4: Dynamic Shard Scaling");
    
    const size_t INITIAL_SHARDS = 4;
    const size_t OPS_PER_PHASE = 50000;
    
    std::cout << "\n  Starting with " << INITIAL_SHARDS << " shards, scaling up dynamically\n\n";
    
    AVLTreeParallelV2<int, int> tree(INITIAL_SHARDS);
    std::mt19937 rng(42);
    std::uniform_int_distribution<> key_dist(0, 999999);
    
    // Fase 1: Operaciones iniciales
    auto ms1 = measure_ms([&] {
        for (size_t i = 0; i < OPS_PER_PHASE; ++i) {
            int key = key_dist(rng);
            tree.insert(key, key);
        }
    });
    
    auto info1 = tree.getArchitectureInfo();
    std::cout << "  Phase 1 (" << info1.num_shards << " shards): " 
              << std::setprecision(2) << ms1 << " ms, "
              << "Balance: " << std::setprecision(1) << (info1.load_balance_score * 100) << "%\n";
    
    // Añadir shards
    tree.add_shard();
    tree.add_shard();
    
    // Fase 2: Más operaciones con más shards
    auto ms2 = measure_ms([&] {
        for (size_t i = 0; i < OPS_PER_PHASE; ++i) {
            int key = key_dist(rng);
            tree.insert(key, key);
        }
    });
    
    auto info2 = tree.getArchitectureInfo();
    std::cout << "  Phase 2 (" << info2.num_shards << " shards): " 
              << std::setprecision(2) << ms2 << " ms, "
              << "Balance: " << std::setprecision(1) << (info2.load_balance_score * 100) << "%\n";
    
    // Añadir más shards
    tree.add_shard();
    tree.add_shard();
    
    // Fase 3: Aún más shards
    auto ms3 = measure_ms([&] {
        for (size_t i = 0; i < OPS_PER_PHASE; ++i) {
            int key = key_dist(rng);
            tree.insert(key, key);
        }
    });
    
    auto info3 = tree.getArchitectureInfo();
    std::cout << "  Phase 3 (" << info3.num_shards << " shards): " 
              << std::setprecision(2) << ms3 << " ms, "
              << "Balance: " << std::setprecision(1) << (info3.load_balance_score * 100) << "%\n";
    
    std::cout << "\n  Final state:\n";
    std::cout << "    Total elements: " << info3.total_elements << "\n";
    std::cout << "    Schema version: " << info3.schema_version << "\n";
    std::cout << "    R/W ratio: " << std::setprecision(2) << info3.global_read_write_ratio << "\n";
}

// -----------------------------------------------------------------------------
// Benchmark 5: V2 Full Feature Test
// -----------------------------------------------------------------------------

void benchmark_v2_full() {
    print_header("Benchmark 5: AVLTreeParallelV2 Full Feature Test");
    
    const size_t NUM_SHARDS = 8;
    const size_t NUM_THREADS = std::thread::hardware_concurrency();
    const size_t OPS_PER_THREAD = 30000;
    
    AVLTreeParallelV2<int, int> tree(NUM_SHARDS, 
        AVLTreeParallelV2<int, int>::RoutingStrategy::PREDICTIVE, 
        true);  // Enable predictions
    
    std::cout << "\n  Config: " << NUM_SHARDS << " shards, " << NUM_THREADS 
              << " threads, predictive routing enabled\n\n";
    
    std::atomic<size_t> total_ops{0};
    std::atomic<size_t> reads{0};
    std::atomic<size_t> writes{0};
    
    auto worker = [&](int thread_id) {
        std::mt19937 rng(thread_id);
        std::uniform_real_distribution<> op_dist(0.0, 1.0);
        std::uniform_int_distribution<> key_dist(0, 499999);
        
        for (size_t i = 0; i < OPS_PER_THREAD; ++i) {
            int key = key_dist(rng);
            double op = op_dist(rng);
            
            if (op < 0.7) {
                // 70% reads
                tree.contains(key);
                reads.fetch_add(1, std::memory_order_relaxed);
            } else if (op < 0.9) {
                // 20% inserts
                tree.insert(key, key * 10);
                writes.fetch_add(1, std::memory_order_relaxed);
            } else {
                // 10% removes
                tree.remove(key);
                writes.fetch_add(1, std::memory_order_relaxed);
            }
            total_ops.fetch_add(1, std::memory_order_relaxed);
        }
    };
    
    double ms = measure_ms([&] {
        std::vector<std::thread> threads;
        for (size_t t = 0; t < NUM_THREADS; ++t) {
            threads.emplace_back(worker, t);
        }
        for (auto& t : threads) t.join();
    });
    
    print_result("V2 Full (Predictive)", ms, total_ops.load());
    
    auto info = tree.getArchitectureInfo();
    std::cout << "\n  Results:\n";
    std::cout << "    Elements: " << info.total_elements << "\n";
    std::cout << "    Balance: " << std::setprecision(1) << (info.load_balance_score * 100) << "%\n";
    std::cout << "    Reads: " << reads.load() << ", Writes: " << writes.load() << "\n";
    std::cout << "    R/W ratio: " << std::setprecision(2) << info.global_read_write_ratio << "\n";
    
    // Mostrar estadísticas de predicción
    std::cout << "\n  Shard Stats:\n";
    auto stats = tree.getShardStats();
    for (const auto& s : stats) {
        std::cout << "    Shard " << s.shard_id << ": " << s.element_count << " elements";
        if (s.predicted_hotspot) {
            std::cout << " [HOTSPOT " << std::setprecision(0) << (s.hotspot_confidence * 100) << "%]";
        }
        std::cout << "\n";
    }
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

int main() {
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║       PARALLEL AVL - FUTURE FEATURES BENCHMARK               ║\n";
    std::cout << "║  Testing: shared_mutex, Predictive Router, Dynamic Shards    ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
    
    benchmark_shared_mutex();
    benchmark_predictive_router();
    benchmark_hotspot_prediction();
    benchmark_dynamic_shards();
    benchmark_v2_full();
    
    std::cout << "\n╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                    BENCHMARK COMPLETE                        ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n\n";
    
    return 0;
}
