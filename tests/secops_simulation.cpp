/**
 * ╔══════════════════════════════════════════════════════════════════════════════╗
 * ║  SecOps Simulation: Composite Rules Detection Test                           ║
 * ║  Inspired by Google SecOps / Chronicle SIEM detection patterns               ║
 * ╚══════════════════════════════════════════════════════════════════════════════╝
 * 
 * Este test simula ataques avanzados tipo "Low and Slow" que intentan evadir
 * la detección del AdversaryResistantRouter mediante:
 * 
 * 1. StealthAttack: Alterna entre 2 shards con pausas de 50ms para evitar
 *    triggers de rate limiting y detección de patrones consecutivos.
 * 
 * 2. BurstWithCooldown: Ráfagas cortas seguidas de períodos de inactividad.
 * 
 * 3. DistributedStealth: Múltiples threads atacando shards diferentes 
 *    coordinadamente para distribuir la sospecha.
 * 
 * Objetivo: Validar si el router actual detecta estos patrones o si pasan
 * "bajo el radar" de los thresholds configurados.
 */

#include "../include/parallel_avl.hpp"
#include "../include/router.hpp"
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <iomanip>
#include <random>
#include <atomic>
#include <mutex>

using namespace std::chrono;
using namespace std::chrono_literals;

// ============================================================================
// ATTACK SIMULATION FRAMEWORK
// ============================================================================

struct AttackMetrics {
    std::atomic<size_t> total_operations{0};
    std::atomic<size_t> successful_inserts{0};
    std::atomic<size_t> detected_patterns{0};
    std::atomic<size_t> blocked_operations{0};
    steady_clock::time_point start_time;
    steady_clock::time_point end_time;
    
    void reset() {
        total_operations = 0;
        successful_inserts = 0;
        detected_patterns = 0;
        blocked_operations = 0;
    }
    
    double duration_ms() const {
        return duration_cast<milliseconds>(end_time - start_time).count();
    }
    
    double ops_per_second() const {
        double secs = duration_ms() / 1000.0;
        return secs > 0 ? total_operations / secs : 0;
    }
};

// Logger thread-safe para eventos del ataque
class AttackLogger {
private:
    std::mutex mtx_;
    bool verbose_;
    
public:
    explicit AttackLogger(bool verbose = false) : verbose_(verbose) {}
    
    void log(const std::string& event, const std::string& details = "") {
        if (!verbose_) return;
        
        auto now = steady_clock::now();
        auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 100000;
        
        std::lock_guard lock(mtx_);
        std::cout << "[" << std::setw(5) << ms << "ms] " << event;
        if (!details.empty()) {
            std::cout << " - " << details;
        }
        std::cout << std::endl;
    }
};

// ============================================================================
// STEALTH ATTACK: "Low and Slow" Alternating Shard Attack
// ============================================================================
// Estrategia: En lugar de bombardear un solo shard (fácil de detectar),
// alternamos entre 2 shards con pausas de 50ms entre ráfagas.
// Esto intenta:
//   1. Evitar el REDIRECT_COOLDOWN de 100ms
//   2. Distribuir la carga aparente entre shards
//   3. Mantener cada ráfaga bajo MAX_CONSECUTIVE_REDIRECTS
// ============================================================================

class StealthAttack {
private:
    static constexpr size_t BURST_SIZE = 2;           // Ops por ráfaga (bajo threshold de 3)
    static constexpr auto PAUSE_DURATION = 50ms;      // Pausa entre ráfagas
    static constexpr size_t TOTAL_BURSTS = 50;        // Total de ráfagas por thread
    
    size_t target_shard_1_;
    size_t target_shard_2_;
    size_t num_shards_;
    AttackLogger& logger_;
    
public:
    StealthAttack(size_t num_shards, size_t shard1, size_t shard2, AttackLogger& logger)
        : target_shard_1_(shard1)
        , target_shard_2_(shard2)
        , num_shards_(num_shards)
        , logger_(logger)
    {}
    
    template<typename TreeType>
    void execute(TreeType& tree, AttackMetrics& metrics, size_t thread_id) {
        std::mt19937 rng(thread_id * 12345);
        
        for (size_t burst = 0; burst < TOTAL_BURSTS; ++burst) {
            // Alternar entre los dos shards objetivo
            size_t current_shard = (burst % 2 == 0) ? target_shard_1_ : target_shard_2_;
            
            // Ráfaga corta al shard actual
            for (size_t op = 0; op < BURST_SIZE; ++op) {
                // Generar key que hashea al shard objetivo
                int key = static_cast<int>((rng() / num_shards_) * num_shards_ + current_shard);
                
                tree.insert(key, static_cast<int>(burst * BURST_SIZE + op));
                metrics.total_operations.fetch_add(1, std::memory_order_relaxed);
                metrics.successful_inserts.fetch_add(1, std::memory_order_relaxed);
            }
            
            logger_.log("BURST", "shard=" + std::to_string(current_shard) + 
                       " ops=" + std::to_string(BURST_SIZE));
            
            // Pausa para evitar detección
            std::this_thread::sleep_for(PAUSE_DURATION);
        }
    }
    
    static const char* name() { return "StealthAttack (Low & Slow)"; }
    static const char* description() {
        return "Alternates between 2 shards with 50ms pauses to evade rate limiting";
    }
};

// ============================================================================
// BURST WITH COOLDOWN: Ráfagas intensas seguidas de silencio
// ============================================================================

class BurstWithCooldown {
private:
    static constexpr size_t BURST_SIZE = 100;         // Ops por ráfaga
    static constexpr auto COOLDOWN = 500ms;           // Pausa larga entre ráfagas
    static constexpr size_t NUM_BURSTS = 10;
    
    size_t target_shard_;
    size_t num_shards_;
    AttackLogger& logger_;
    
public:
    BurstWithCooldown(size_t num_shards, size_t target, AttackLogger& logger)
        : target_shard_(target)
        , num_shards_(num_shards)
        , logger_(logger)
    {}
    
    template<typename TreeType>
    void execute(TreeType& tree, AttackMetrics& metrics, size_t thread_id) {
        std::mt19937 rng(thread_id * 54321);
        
        for (size_t burst = 0; burst < NUM_BURSTS; ++burst) {
            logger_.log("BURST_START", "burst=" + std::to_string(burst));
            
            // Ráfaga intensa
            for (size_t op = 0; op < BURST_SIZE; ++op) {
                int key = static_cast<int>((rng() / num_shards_) * num_shards_ + target_shard_);
                tree.insert(key, static_cast<int>(op));
                metrics.total_operations.fetch_add(1, std::memory_order_relaxed);
            }
            
            logger_.log("BURST_END", "cooling down for 500ms");
            
            // Cooldown largo para "resetear" contadores del router
            std::this_thread::sleep_for(COOLDOWN);
        }
    }
    
    static const char* name() { return "BurstWithCooldown"; }
    static const char* description() {
        return "Intense bursts followed by 500ms cooldown to reset detection counters";
    }
};

// ============================================================================
// DISTRIBUTED STEALTH: Ataque coordinado multi-thread
// ============================================================================

class DistributedStealth {
private:
    static constexpr size_t OPS_PER_WAVE = 10;
    static constexpr auto WAVE_INTERVAL = 25ms;
    static constexpr size_t NUM_WAVES = 40;
    
    size_t num_shards_;
    AttackLogger& logger_;
    std::atomic<size_t>& sync_counter_;
    
public:
    DistributedStealth(size_t num_shards, AttackLogger& logger, std::atomic<size_t>& sync)
        : num_shards_(num_shards)
        , logger_(logger)
        , sync_counter_(sync)
    {}
    
    template<typename TreeType>
    void execute(TreeType& tree, AttackMetrics& metrics, size_t thread_id) {
        std::mt19937 rng(thread_id * 98765);
        
        // Cada thread ataca un shard diferente en cada wave
        for (size_t wave = 0; wave < NUM_WAVES; ++wave) {
            size_t my_shard = (thread_id + wave) % num_shards_;
            
            // Operaciones de esta wave
            for (size_t op = 0; op < OPS_PER_WAVE; ++op) {
                int key = static_cast<int>(rng() % 10000);
                // Forzar al shard asignado
                key = (key / static_cast<int>(num_shards_)) * static_cast<int>(num_shards_) 
                      + static_cast<int>(my_shard);
                
                tree.insert(key, static_cast<int>(wave * OPS_PER_WAVE + op));
                metrics.total_operations.fetch_add(1, std::memory_order_relaxed);
            }
            
            // Sincronización ligera entre threads
            sync_counter_.fetch_add(1, std::memory_order_release);
            
            std::this_thread::sleep_for(WAVE_INTERVAL);
        }
    }
    
    static const char* name() { return "DistributedStealth"; }
    static const char* description() {
        return "Coordinated multi-thread attack rotating target shards each wave";
    }
};

// ============================================================================
// TEST RUNNER
// ============================================================================

class SecOpsSimulation {
private:
    static constexpr size_t NUM_SHARDS = 8;
    static constexpr size_t NUM_THREADS = 4;
    
    AttackLogger logger_;
    bool verbose_;
    
    void print_header(const char* attack_name, const char* description) {
        std::cout << "\n";
        std::cout << "┌──────────────────────────────────────────────────────────────┐\n";
        std::cout << "│ " << std::left << std::setw(60) << attack_name << " │\n";
        std::cout << "├──────────────────────────────────────────────────────────────┤\n";
        std::cout << "│ " << std::left << std::setw(60) << description << " │\n";
        std::cout << "└──────────────────────────────────────────────────────────────┘\n";
    }
    
    void print_results(const AttackMetrics& metrics, 
                       const typename ParallelAVL<int, int>::Stats& stats,
                       const char* attack_name) {
        
        bool detected = stats.suspicious_patterns > 0 || stats.blocked_redirects > 0;
        bool evaded = !detected && stats.balance_score < 0.7;
        
        std::cout << "\n  📊 RESULTADOS:\n";
        std::cout << "  ├─ Operaciones totales:  " << metrics.total_operations << "\n";
        std::cout << "  ├─ Throughput:           " << std::fixed << std::setprecision(0) 
                  << metrics.ops_per_second() << " ops/s\n";
        std::cout << "  ├─ Duración:             " << metrics.duration_ms() << " ms\n";
        std::cout << "  │\n";
        std::cout << "  ├─ Balance Score:        " << std::fixed << std::setprecision(1) 
                  << (stats.balance_score * 100) << "%\n";
        std::cout << "  ├─ Hotspot detectado:    " << (stats.has_hotspot ? "SÍ ⚠️" : "No") << "\n";
        std::cout << "  ├─ Patrones sospechosos: " << stats.suspicious_patterns << "\n";
        std::cout << "  ├─ Redirects bloqueados: " << stats.blocked_redirects << "\n";
        std::cout << "  │\n";
        
        // Distribución de shards
        std::cout << "  └─ Distribución: [";
        for (size_t i = 0; i < stats.shard_sizes.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << stats.shard_sizes[i];
        }
        std::cout << "]\n";
        
        // Veredicto
        std::cout << "\n  ";
        if (detected) {
            std::cout << "✅ DETECTADO: El router identificó el ataque\n";
            std::cout << "     Router defense: WORKING\n";
        } else if (evaded) {
            std::cout << "❌ EVASIÓN: El ataque pasó bajo el radar!\n";
            std::cout << "     Balance Score bajo sin detección = defensa evadida\n";
            std::cout << "     ⚠️  Se requieren composite rules más sofisticadas\n";
        } else {
            std::cout << "⚪ NEUTRAL: Ataque no causó desbalance significativo\n";
            std::cout << "     Puede indicar que el ataque no fue efectivo o\n";
            std::cout << "     que la estrategia de routing absorbió el impacto\n";
        }
    }
    
public:
    explicit SecOpsSimulation(bool verbose = false) 
        : logger_(verbose), verbose_(verbose) {}
    
    void run_stealth_attack() {
        print_header(StealthAttack::name(), StealthAttack::description());
        
        ParallelAVL<int, int> tree(NUM_SHARDS, 
            ParallelAVL<int, int>::RouterStrategy::INTELLIGENT);
        
        AttackMetrics metrics;
        metrics.start_time = steady_clock::now();
        
        std::vector<std::thread> threads;
        for (size_t tid = 0; tid < NUM_THREADS; ++tid) {
            threads.emplace_back([&, tid]() {
                // Cada thread alterna entre shards 0-1 o 2-3
                size_t shard1 = (tid % 2) * 2;
                size_t shard2 = shard1 + 1;
                
                StealthAttack attack(NUM_SHARDS, shard1, shard2, logger_);
                attack.execute(tree, metrics, tid);
            });
        }
        
        for (auto& t : threads) t.join();
        
        metrics.end_time = steady_clock::now();
        auto stats = tree.get_stats();
        
        print_results(metrics, stats, StealthAttack::name());
    }
    
    void run_burst_with_cooldown() {
        print_header(BurstWithCooldown::name(), BurstWithCooldown::description());
        
        ParallelAVL<int, int> tree(NUM_SHARDS,
            ParallelAVL<int, int>::RouterStrategy::INTELLIGENT);
        
        AttackMetrics metrics;
        metrics.start_time = steady_clock::now();
        
        std::vector<std::thread> threads;
        for (size_t tid = 0; tid < NUM_THREADS; ++tid) {
            threads.emplace_back([&, tid]() {
                // Todos apuntan al shard 0
                BurstWithCooldown attack(NUM_SHARDS, 0, logger_);
                attack.execute(tree, metrics, tid);
            });
        }
        
        for (auto& t : threads) t.join();
        
        metrics.end_time = steady_clock::now();
        auto stats = tree.get_stats();
        
        print_results(metrics, stats, BurstWithCooldown::name());
    }
    
    void run_distributed_stealth() {
        print_header(DistributedStealth::name(), DistributedStealth::description());
        
        ParallelAVL<int, int> tree(NUM_SHARDS,
            ParallelAVL<int, int>::RouterStrategy::INTELLIGENT);
        
        AttackMetrics metrics;
        std::atomic<size_t> sync_counter{0};
        metrics.start_time = steady_clock::now();
        
        std::vector<std::thread> threads;
        for (size_t tid = 0; tid < NUM_THREADS; ++tid) {
            threads.emplace_back([&, tid]() {
                DistributedStealth attack(NUM_SHARDS, logger_, sync_counter);
                attack.execute(tree, metrics, tid);
            });
        }
        
        for (auto& t : threads) t.join();
        
        metrics.end_time = steady_clock::now();
        auto stats = tree.get_stats();
        
        print_results(metrics, stats, DistributedStealth::name());
    }
    
    void run_comparison_with_strategies() {
        std::cout << "\n";
        std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
        std::cout << "║  COMPARACIÓN: StealthAttack vs Diferentes Estrategias       ║\n";
        std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
        
        std::vector<std::pair<std::string, typename ParallelAVL<int, int>::RouterStrategy>> strategies = {
            {"Static Hash", ParallelAVL<int, int>::RouterStrategy::STATIC_HASH},
            {"Load-Aware", ParallelAVL<int, int>::RouterStrategy::LOAD_AWARE},
            {"Virtual Nodes", ParallelAVL<int, int>::RouterStrategy::VIRTUAL_NODES},
            {"Intelligent", ParallelAVL<int, int>::RouterStrategy::INTELLIGENT},
        };
        
        std::cout << "\n  " << std::left 
                  << std::setw(15) << "Strategy"
                  << std::setw(12) << "Balance"
                  << std::setw(10) << "Hotspot"
                  << std::setw(12) << "Suspicious"
                  << std::setw(10) << "Blocked"
                  << "Verdict\n";
        std::cout << "  " << std::string(70, '-') << "\n";
        
        for (const auto& [name, strategy] : strategies) {
            ParallelAVL<int, int> tree(NUM_SHARDS, strategy);
            AttackMetrics metrics;
            
            // Ejecutar stealth attack
            std::vector<std::thread> threads;
            for (size_t tid = 0; tid < NUM_THREADS; ++tid) {
                threads.emplace_back([&, tid]() {
                    size_t shard1 = (tid % 2) * 2;
                    size_t shard2 = shard1 + 1;
                    StealthAttack attack(NUM_SHARDS, shard1, shard2, logger_);
                    attack.execute(tree, metrics, tid);
                });
            }
            for (auto& t : threads) t.join();
            
            auto stats = tree.get_stats();
            
            bool detected = stats.suspicious_patterns > 0 || stats.blocked_redirects > 0;
            bool evaded = !detected && stats.balance_score < 0.7;
            
            std::string verdict;
            if (detected) verdict = "✅ DETECTED";
            else if (evaded) verdict = "❌ EVADED";
            else verdict = "⚪ NEUTRAL";
            
            std::cout << "  " << std::left
                      << std::setw(15) << name
                      << std::setw(12) << (std::to_string(static_cast<int>(stats.balance_score * 100)) + "%")
                      << std::setw(10) << (stats.has_hotspot ? "YES" : "No")
                      << std::setw(12) << stats.suspicious_patterns
                      << std::setw(10) << stats.blocked_redirects
                      << verdict << "\n";
        }
    }
    
    void run_all() {
        std::cout << "\n";
        std::cout << "╔══════════════════════════════════════════════════════════════════╗\n";
        std::cout << "║  SecOps Simulation: Composite Rules Detection Test               ║\n";
        std::cout << "║  Testing Advanced Evasion Techniques Against Router Defense      ║\n";
        std::cout << "╚══════════════════════════════════════════════════════════════════╝\n";
        
        run_stealth_attack();
        run_burst_with_cooldown();
        run_distributed_stealth();
        run_comparison_with_strategies();
        
        std::cout << "\n";
        std::cout << "╔══════════════════════════════════════════════════════════════════╗\n";
        std::cout << "║  SecOps Simulation Complete                                      ║\n";
        std::cout << "╚══════════════════════════════════════════════════════════════════╝\n";
        
        std::cout << "\n  📋 RECOMENDACIONES PARA MEJORAR DETECCIÓN:\n";
        std::cout << "  \n";
        std::cout << "  1. Composite Rules: Correlacionar eventos entre múltiples shards\n";
        std::cout << "     - Si shard A y shard B reciben tráfico coordinado → sospechoso\n";
        std::cout << "  \n";
        std::cout << "  2. Sliding Window Detection: En lugar de cooldown fijo,\n";
        std::cout << "     usar ventana deslizante para detectar patrones distribuidos\n";
        std::cout << "  \n";
        std::cout << "  3. Entropy Analysis: Medir la entropía de la distribución\n";
        std::cout << "     de keys por shard; baja entropía = ataque dirigido\n";
        std::cout << "  \n";
        std::cout << "  4. Rate Limiting Global: Limitar ops/segundo globales,\n";
        std::cout << "     no solo por key individual\n";
        std::cout << "\n";
    }
};

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char* argv[]) {
    bool verbose = false;
    
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "-v" || std::string(argv[i]) == "--verbose") {
            verbose = true;
        }
    }
    
    SecOpsSimulation simulation(verbose);
    simulation.run_all();
    
    return 0;
}
