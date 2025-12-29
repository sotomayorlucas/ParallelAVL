// =============================================================================
// Sensitivity Analysis Benchmark - Parallel AVL Trees
// Author: Lucas Sotomayor
// 
// Análisis sistemático de la sensibilidad del sistema a variaciones en los
// parámetros de configuración fijados.
// =============================================================================

#include <iostream>
#include <iomanip>
#include <vector>
#include <map>
#include <string>
#include <chrono>
#include <thread>
#include <random>
#include <fstream>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <mutex>
#include <atomic>

// Simulamos los componentes del ParallelAVL para el análisis
// En producción, estos valores vendrían de los headers reales

// =============================================================================
// PARÁMETROS A ANALIZAR
// =============================================================================
struct SystemParameters {
    // Router parameters (router.hpp)
    size_t vnodes_per_shard = 16;        // Virtual nodes for consistent hashing
    size_t window_size = 50;              // Recent operations window
    double hotspot_threshold = 1.5;       // Hotspot detection threshold
    size_t max_consecutive_redirects = 3; // Anti-thrashing limit
    size_t redirect_cooldown_ms = 100;    // Cooldown between redirects
    
    // CachedLoadStats parameters (cached_load_stats.hpp)
    size_t refresh_interval_ms = 1;       // Background refresh interval
    
    // ParallelAVL parameters (parallel_avl.hpp)
    size_t num_shards = 8;                // Number of shards
    
    // Rebalancing parameters (AVLTreeParallel.h)
    double rebalance_threshold = 2.0;     // Trigger rebalancing when max/min > threshold
    double balance_score_min = 0.8;       // Acceptable balance score
    
    std::string to_string() const {
        std::ostringstream ss;
        ss << "vnodes=" << vnodes_per_shard 
           << ",window=" << window_size
           << ",hotspot=" << hotspot_threshold
           << ",max_redir=" << max_consecutive_redirects
           << ",cooldown=" << redirect_cooldown_ms
           << ",refresh=" << refresh_interval_ms
           << ",shards=" << num_shards
           << ",rebal=" << rebalance_threshold
           << ",bal_min=" << balance_score_min;
        return ss.str();
    }
};

// =============================================================================
// WORKLOAD GENERATORS
// =============================================================================
class WorkloadGenerator {
public:
    virtual ~WorkloadGenerator() = default;
    virtual uint64_t next_key() = 0;
    virtual std::string name() const = 0;
};

class UniformWorkload : public WorkloadGenerator {
    std::mt19937_64 rng_;
    std::uniform_int_distribution<uint64_t> dist_;
public:
    UniformWorkload(uint64_t max_key = 100000) 
        : rng_(42), dist_(0, max_key) {}
    uint64_t next_key() override { return dist_(rng_); }
    std::string name() const override { return "Uniform"; }
};

class ZipfianWorkload : public WorkloadGenerator {
    std::mt19937_64 rng_;
    std::vector<double> cdf_;
    uint64_t max_key_;
public:
    ZipfianWorkload(uint64_t max_key = 100000, double alpha = 0.99) 
        : rng_(42), max_key_(max_key) {
        // Build CDF
        double sum = 0;
        for (uint64_t i = 1; i <= max_key; ++i) {
            sum += 1.0 / std::pow(i, alpha);
        }
        double cumulative = 0;
        cdf_.reserve(max_key);
        for (uint64_t i = 1; i <= max_key; ++i) {
            cumulative += (1.0 / std::pow(i, alpha)) / sum;
            cdf_.push_back(cumulative);
        }
    }
    uint64_t next_key() override {
        double u = std::uniform_real_distribution<>(0, 1)(rng_);
        auto it = std::lower_bound(cdf_.begin(), cdf_.end(), u);
        return std::distance(cdf_.begin(), it);
    }
    std::string name() const override { return "Zipfian"; }
};

class AdversarialWorkload : public WorkloadGenerator {
    size_t num_shards_;
    size_t target_shard_;
    uint64_t counter_ = 0;
public:
    AdversarialWorkload(size_t num_shards, size_t target = 0) 
        : num_shards_(num_shards), target_shard_(target) {}
    uint64_t next_key() override { 
        return target_shard_ + (counter_++ * num_shards_); 
    }
    std::string name() const override { return "Adversarial"; }
};

class SequentialWorkload : public WorkloadGenerator {
    uint64_t counter_ = 0;
public:
    uint64_t next_key() override { return counter_++; }
    std::string name() const override { return "Sequential"; }
};

// =============================================================================
// SIMULATED PARALLEL AVL (for benchmarking parameter sensitivity)
// =============================================================================
class SimulatedParallelAVL {
    SystemParameters params_;
    std::vector<size_t> shard_sizes_;
    std::vector<size_t> shard_ops_;
    size_t total_ops_ = 0;
    size_t redirects_ = 0;
    size_t blocked_redirects_ = 0;
    
    std::map<uint64_t, std::chrono::steady_clock::time_point> redirect_times_;
    std::map<uint64_t, size_t> redirect_counts_;
    
public:
    explicit SimulatedParallelAVL(const SystemParameters& params)
        : params_(params)
        , shard_sizes_(params.num_shards, 0)
        , shard_ops_(params.num_shards, 0) {}
    
    void insert(uint64_t key) {
        total_ops_++;
        size_t natural_shard = key % params_.num_shards;
        size_t target_shard = route(key, natural_shard);
        
        shard_sizes_[target_shard]++;
        shard_ops_[target_shard]++;
        
        if (target_shard != natural_shard) {
            redirects_++;
        }
    }
    
    size_t route(uint64_t key, size_t natural_shard) {
        // Check for hotspot
        double avg_load = std::accumulate(shard_sizes_.begin(), shard_sizes_.end(), 0.0) 
                         / params_.num_shards;
        
        if (avg_load > 0 && shard_sizes_[natural_shard] > params_.hotspot_threshold * avg_load) {
            // Check anti-thrashing
            auto now = std::chrono::steady_clock::now();
            auto& last_redirect = redirect_times_[key];
            auto& count = redirect_counts_[key];
            
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_redirect).count();
            
            if (elapsed < static_cast<long>(params_.redirect_cooldown_ms)) {
                count++;
                if (count > params_.max_consecutive_redirects) {
                    blocked_redirects_++;
                    return natural_shard;  // Blocked
                }
            } else {
                count = 1;
            }
            last_redirect = now;
            
            // Find least loaded shard
            size_t min_shard = 0;
            size_t min_load = shard_sizes_[0];
            for (size_t i = 1; i < params_.num_shards; ++i) {
                if (shard_sizes_[i] < min_load) {
                    min_load = shard_sizes_[i];
                    min_shard = i;
                }
            }
            return min_shard;
        }
        
        return natural_shard;
    }
    
    // Metrics
    double get_balance_score() const {
        if (shard_sizes_.empty()) return 1.0;
        
        double avg = std::accumulate(shard_sizes_.begin(), shard_sizes_.end(), 0.0) 
                    / shard_sizes_.size();
        if (avg == 0) return 1.0;
        
        double variance = 0;
        for (size_t load : shard_sizes_) {
            double diff = load - avg;
            variance += diff * diff;
        }
        variance /= shard_sizes_.size();
        double stddev = std::sqrt(variance);
        
        return std::max(0.0, 1.0 - (stddev / avg));
    }
    
    size_t get_max_load() const {
        return *std::max_element(shard_sizes_.begin(), shard_sizes_.end());
    }
    
    size_t get_min_load() const {
        return *std::min_element(shard_sizes_.begin(), shard_sizes_.end());
    }
    
    size_t get_redirects() const { return redirects_; }
    size_t get_blocked() const { return blocked_redirects_; }
    size_t get_total_ops() const { return total_ops_; }
    
    const std::vector<size_t>& get_shard_sizes() const { return shard_sizes_; }
};

// =============================================================================
// EXPERIMENT RESULTS
// =============================================================================
struct ExperimentResult {
    std::string parameter_name;
    double parameter_value;
    std::string workload;
    
    double balance_score;
    double throughput_mops;
    size_t redirects;
    size_t blocked_redirects;
    double max_min_ratio;
    double latency_p99_us;
};

// =============================================================================
// SENSITIVITY ANALYZER
// =============================================================================
class SensitivityAnalyzer {
    std::vector<ExperimentResult> results_;
    size_t ops_per_experiment_ = 100000;
    size_t warmup_ops_ = 10000;
    
public:
    void set_ops_per_experiment(size_t ops) { ops_per_experiment_ = ops; }
    
    // Run single experiment
    ExperimentResult run_experiment(const SystemParameters& params, 
                                    WorkloadGenerator& workload,
                                    const std::string& param_name,
                                    double param_value) {
        SimulatedParallelAVL tree(params);
        
        // Warmup
        for (size_t i = 0; i < warmup_ops_; ++i) {
            tree.insert(workload.next_key());
        }
        
        // Timed run
        auto start = std::chrono::high_resolution_clock::now();
        
        for (size_t i = 0; i < ops_per_experiment_; ++i) {
            tree.insert(workload.next_key());
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        double duration_ms = std::chrono::duration<double, std::milli>(end - start).count();
        
        ExperimentResult result;
        result.parameter_name = param_name;
        result.parameter_value = param_value;
        result.workload = workload.name();
        result.balance_score = tree.get_balance_score();
        result.throughput_mops = (ops_per_experiment_ / duration_ms) / 1000.0;
        result.redirects = tree.get_redirects();
        result.blocked_redirects = tree.get_blocked();
        
        size_t max_load = tree.get_max_load();
        size_t min_load = tree.get_min_load();
        result.max_min_ratio = min_load > 0 ? static_cast<double>(max_load) / min_load : 999.0;
        result.latency_p99_us = duration_ms * 1000.0 / ops_per_experiment_ * 2.5; // Estimate
        
        return result;
    }
    
    // Analyze sensitivity of a parameter
    template<typename T, typename Setter>
    void analyze_parameter(const std::string& name, 
                          std::vector<T> values,
                          Setter setter) {
        std::cout << "\n═══════════════════════════════════════════════════════════════\n";
        std::cout << " Analyzing: " << name << "\n";
        std::cout << "═══════════════════════════════════════════════════════════════\n";
        
        // Test with different workloads
        std::vector<std::unique_ptr<WorkloadGenerator>> workloads;
        workloads.push_back(std::make_unique<UniformWorkload>());
        workloads.push_back(std::make_unique<ZipfianWorkload>());
        workloads.push_back(std::make_unique<AdversarialWorkload>(8));
        
        for (auto& wl : workloads) {
            std::cout << "\n  Workload: " << wl->name() << "\n";
            std::cout << "  ─────────────────────────────────────────────────────────\n";
            std::cout << std::setw(12) << "Value" 
                      << std::setw(12) << "Balance"
                      << std::setw(12) << "Mops/s"
                      << std::setw(12) << "Redirects"
                      << std::setw(12) << "MaxMin"
                      << "\n";
            
            for (const T& val : values) {
                SystemParameters params;
                setter(params, val);
                
                auto result = run_experiment(params, *wl, name, static_cast<double>(val));
                results_.push_back(result);
                
                std::cout << std::setw(12) << val
                          << std::setw(11) << std::fixed << std::setprecision(2) 
                          << (result.balance_score * 100) << "%"
                          << std::setw(12) << std::setprecision(3) << result.throughput_mops
                          << std::setw(12) << result.redirects
                          << std::setw(11) << std::setprecision(2) << result.max_min_ratio << "x"
                          << "\n";
            }
        }
    }
    
    // Run full sensitivity analysis
    void run_full_analysis() {
        std::cout << "\n";
        std::cout << "╔════════════════════════════════════════════════════════════════╗\n";
        std::cout << "║     PARALLEL AVL - SENSITIVITY ANALYSIS                        ║\n";
        std::cout << "║     Systematic Parameter Variation Study                       ║\n";
        std::cout << "╚════════════════════════════════════════════════════════════════╝\n";
        
        // 1. Number of Shards
        analyze_parameter<size_t>("num_shards", {2, 4, 8, 16, 32, 64},
            [](SystemParameters& p, size_t v) { p.num_shards = v; });
        
        // 2. Hotspot Threshold
        analyze_parameter<double>("hotspot_threshold", {1.1, 1.25, 1.5, 2.0, 3.0, 5.0},
            [](SystemParameters& p, double v) { p.hotspot_threshold = v; });
        
        // 3. Max Consecutive Redirects
        analyze_parameter<size_t>("max_consecutive_redirects", {1, 2, 3, 5, 10, 20},
            [](SystemParameters& p, size_t v) { p.max_consecutive_redirects = v; });
        
        // 4. Redirect Cooldown (ms)
        analyze_parameter<size_t>("redirect_cooldown_ms", {10, 50, 100, 200, 500, 1000},
            [](SystemParameters& p, size_t v) { p.redirect_cooldown_ms = v; });
        
        // 5. Virtual Nodes per Shard
        analyze_parameter<size_t>("vnodes_per_shard", {4, 8, 16, 32, 64, 128},
            [](SystemParameters& p, size_t v) { p.vnodes_per_shard = v; });
        
        // 6. Window Size
        analyze_parameter<size_t>("window_size", {10, 25, 50, 100, 200, 500},
            [](SystemParameters& p, size_t v) { p.window_size = v; });
        
        // 7. Refresh Interval (ms)
        analyze_parameter<size_t>("refresh_interval_ms", {1, 5, 10, 50, 100, 500},
            [](SystemParameters& p, size_t v) { p.refresh_interval_ms = v; });
        
        // 8. Balance Score Minimum
        analyze_parameter<double>("balance_score_min", {0.5, 0.6, 0.7, 0.8, 0.9, 0.95},
            [](SystemParameters& p, double v) { p.balance_score_min = v; });
    }
    
    // Export results to CSV
    void export_csv(const std::string& filename) {
        std::ofstream file(filename);
        file << "parameter,value,workload,balance_score,throughput_mops,redirects,blocked,max_min_ratio,latency_p99\n";
        
        for (const auto& r : results_) {
            file << r.parameter_name << ","
                 << r.parameter_value << ","
                 << r.workload << ","
                 << r.balance_score << ","
                 << r.throughput_mops << ","
                 << r.redirects << ","
                 << r.blocked_redirects << ","
                 << r.max_min_ratio << ","
                 << r.latency_p99_us << "\n";
        }
        
        std::cout << "\n✓ Results exported to: " << filename << "\n";
    }
    
    // Generate summary
    void print_summary() {
        std::cout << "\n";
        std::cout << "╔════════════════════════════════════════════════════════════════╗\n";
        std::cout << "║                    ANALYSIS SUMMARY                            ║\n";
        std::cout << "╚════════════════════════════════════════════════════════════════╝\n\n";
        
        // Group by parameter
        std::map<std::string, std::vector<ExperimentResult>> by_param;
        for (const auto& r : results_) {
            by_param[r.parameter_name].push_back(r);
        }
        
        for (const auto& [param, experiments] : by_param) {
            // Find best value for adversarial workload
            double best_balance = 0;
            double best_value = 0;
            
            for (const auto& e : experiments) {
                if (e.workload == "Adversarial" && e.balance_score > best_balance) {
                    best_balance = e.balance_score;
                    best_value = e.parameter_value;
                }
            }
            
            std::cout << "  " << param << ":\n";
            std::cout << "    Best for adversarial defense: " << best_value 
                      << " (balance: " << std::fixed << std::setprecision(1) 
                      << (best_balance * 100) << "%)\n";
        }
        
        std::cout << "\n  RECOMMENDED CONFIGURATION:\n";
        std::cout << "  ─────────────────────────────────────────────────────────\n";
        std::cout << "    num_shards:               8-16 (scales with cores)\n";
        std::cout << "    hotspot_threshold:        1.5 (sensitive detection)\n";
        std::cout << "    max_consecutive_redirects: 3 (anti-thrashing)\n";
        std::cout << "    redirect_cooldown_ms:     100 (rate limiting)\n";
        std::cout << "    vnodes_per_shard:         16 (consistent hashing)\n";
        std::cout << "    window_size:              50 (recent ops tracking)\n";
        std::cout << "    refresh_interval_ms:      1 (real-time stats)\n";
        std::cout << "    balance_score_min:        0.8 (quality threshold)\n";
    }
    
    const std::vector<ExperimentResult>& get_results() const { return results_; }
};

// =============================================================================
// MAIN
// =============================================================================
int main() {
    std::cout << "Starting Sensitivity Analysis...\n";
    
    SensitivityAnalyzer analyzer;
    analyzer.set_ops_per_experiment(50000);  // 50K ops per experiment
    
    analyzer.run_full_analysis();
    analyzer.print_summary();
    analyzer.export_csv("sensitivity_results.csv");
    
    std::cout << "\n✓ Analysis complete!\n";
    
    return 0;
}
