/**
 * Compiler Comparison Benchmark
 * 
 * Uses the ORIGINAL ParallelAVL implementation (not Intel-optimized simulation)
 * to provide a fair comparison between GCC and ICX compilers.
 * 
 * Key differences from intel_optimized_bench.cpp:
 * - Uses std::map as backing store (similar to original AVL)
 * - No Intel-specific optimizations in the code
 * - Standard C++ threading without OpenMP intrinsics
 * - Generic implementation suitable for any compiler
 */

#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include <algorithm>
#include <numeric>
#include <iomanip>
#include <fstream>
#include <mutex>
#include <map>
#include <cmath>
#include <functional>

// ============================================================================
// ORIGINAL-STYLE ParallelAVL Implementation
// This mirrors the actual project structure without Intel optimizations
// ============================================================================

template<typename Key, typename Value>
class OriginalParallelAVL {
private:
    struct Shard {
        std::mutex mtx;
        std::map<Key, Value> tree;  // Using std::map as AVL substitute
        std::atomic<size_t> ops_count{0};
        std::atomic<size_t> size{0};
    };
    
    std::vector<Shard> shards_;
    size_t num_shards_;
    std::atomic<size_t> total_ops_{0};
    std::atomic<size_t> redirects_{0};
    
    // Original routing parameters (from router.hpp)
    static constexpr size_t VNODES_PER_SHARD = 16;
    static constexpr double HOTSPOT_THRESHOLD = 1.5;
    static constexpr size_t MAX_CONSECUTIVE_REDIRECTS = 3;
    
    // Load tracking for adaptive routing
    std::vector<std::atomic<size_t>> recent_loads_;
    
public:
    explicit OriginalParallelAVL(size_t num_shards) 
        : shards_(num_shards), num_shards_(num_shards), recent_loads_(num_shards) {
        for (auto& load : recent_loads_) load = 0;
    }
    
    // Original consistent hashing with virtual nodes
    size_t route(const Key& key) {
        std::hash<Key> hasher;
        size_t hash = hasher(key);
        
        // Virtual node selection (original algorithm)
        size_t vnode = hash % (num_shards_ * VNODES_PER_SHARD);
        size_t primary_shard = vnode / VNODES_PER_SHARD;
        
        // Load-aware redirection (original logic)
        recent_loads_[primary_shard].fetch_add(1, std::memory_order_relaxed);
        
        // Check for hotspot
        size_t total_load = 0;
        for (size_t i = 0; i < num_shards_; i++) {
            total_load += recent_loads_[i].load(std::memory_order_relaxed);
        }
        
        double avg_load = static_cast<double>(total_load) / num_shards_;
        double shard_load = recent_loads_[primary_shard].load(std::memory_order_relaxed);
        
        if (avg_load > 0 && shard_load > avg_load * HOTSPOT_THRESHOLD) {
            // Redirect to least loaded shard
            size_t min_load = SIZE_MAX;
            size_t target = primary_shard;
            for (size_t i = 0; i < num_shards_; i++) {
                size_t load = recent_loads_[i].load(std::memory_order_relaxed);
                if (load < min_load) {
                    min_load = load;
                    target = i;
                }
            }
            if (target != primary_shard) {
                redirects_.fetch_add(1, std::memory_order_relaxed);
                return target;
            }
        }
        
        return primary_shard;
    }
    
    bool insert(const Key& key, const Value& value) {
        size_t shard_id = route(key);
        std::lock_guard<std::mutex> lock(shards_[shard_id].mtx);
        shards_[shard_id].tree[key] = value;
        shards_[shard_id].ops_count++;
        shards_[shard_id].size++;
        total_ops_++;
        return true;
    }
    
    bool contains(const Key& key) {
        size_t shard_id = route(key);
        std::lock_guard<std::mutex> lock(shards_[shard_id].mtx);
        shards_[shard_id].ops_count++;
        total_ops_++;
        return shards_[shard_id].tree.count(key) > 0;
    }
    
    size_t get_total_ops() const { return total_ops_.load(); }
    size_t get_redirects() const { return redirects_.load(); }
    
    double get_balance_score() const {
        std::vector<size_t> loads;
        for (size_t i = 0; i < num_shards_; i++) {
            loads.push_back(shards_[i].ops_count.load());
        }
        if (loads.empty()) return 1.0;
        
        double mean = std::accumulate(loads.begin(), loads.end(), 0.0) / loads.size();
        if (mean == 0) return 1.0;
        
        double sq_sum = 0;
        for (auto l : loads) sq_sum += (l - mean) * (l - mean);
        double stddev = std::sqrt(sq_sum / loads.size());
        
        return std::max(0.0, 1.0 - (stddev / mean));
    }
    
    void reset() {
        total_ops_ = 0;
        redirects_ = 0;
        for (auto& s : shards_) {
            s.ops_count = 0;
            s.size = 0;
            s.tree.clear();
        }
        for (auto& l : recent_loads_) l = 0;
    }
};

// ============================================================================
// Workload Generators (Original implementation)
// ============================================================================

class WorkloadGenerator {
public:
    virtual ~WorkloadGenerator() = default;
    virtual int64_t next() = 0;
    virtual std::string name() const = 0;
    virtual void reset() {}
};

class UniformWorkload : public WorkloadGenerator {
    std::mt19937_64 rng_;
    std::uniform_int_distribution<int64_t> dist_;
public:
    explicit UniformWorkload(int64_t max_key = 100000) 
        : rng_(std::random_device{}()), dist_(0, max_key) {}
    int64_t next() override { return dist_(rng_); }
    std::string name() const override { return "Uniform"; }
};

class ZipfianWorkload : public WorkloadGenerator {
    std::mt19937_64 rng_;
    std::vector<double> cdf_;
    int64_t n_;
public:
    explicit ZipfianWorkload(int64_t n = 100000, double alpha = 0.99) 
        : rng_(std::random_device{}()), n_(n) {
        double sum = 0;
        cdf_.resize(n);
        for (int64_t i = 1; i <= n; i++) {
            sum += 1.0 / std::pow(i, alpha);
            cdf_[i-1] = sum;
        }
        for (auto& v : cdf_) v /= sum;
    }
    int64_t next() override {
        double r = std::uniform_real_distribution<>(0, 1)(rng_);
        return std::lower_bound(cdf_.begin(), cdf_.end(), r) - cdf_.begin();
    }
    std::string name() const override { return "Zipfian"; }
};

class AdversarialWorkload : public WorkloadGenerator {
    size_t num_shards_;
    std::atomic<int64_t> counter_{0};
public:
    explicit AdversarialWorkload(size_t num_shards) : num_shards_(num_shards) {}
    int64_t next() override { return counter_.fetch_add(1) * num_shards_; }
    std::string name() const override { return "Adversarial"; }
};

// ============================================================================
// Benchmark Runner
// ============================================================================

struct BenchResult {
    std::string test_name;
    double throughput_mops;
    double balance_score;
    double avg_latency_ns;
    size_t redirects;
};

class CompilerBenchmark {
private:
    static constexpr size_t WARMUP_OPS = 10000;
    static constexpr size_t OPS_PER_THREAD = 50000;
    
public:
    // Scalability test
    std::vector<BenchResult> run_scalability(int max_threads = 16) {
        std::vector<BenchResult> results;
        
        std::cout << "\n=== SCALABILITY (Original Implementation) ===\n";
        std::cout << std::setw(10) << "Threads" << std::setw(12) << "Shards"
                  << std::setw(15) << "Throughput" << std::setw(12) << "Balance"
                  << std::setw(12) << "Redirects\n";
        std::cout << std::string(60, '-') << "\n";
        
        for (int threads : {1, 2, 4, 8, 12, 16}) {
            if (threads > max_threads) break;
            
            int shards = std::max(threads, 8);
            OriginalParallelAVL<int64_t, int64_t> tree(shards);
            
            std::vector<std::thread> workers;
            std::atomic<bool> start{false};
            
            for (int t = 0; t < threads; t++) {
                workers.emplace_back([&, t]() {
                    std::mt19937_64 rng(t + 42);
                    std::uniform_int_distribution<int64_t> dist(0, 1000000);
                    
                    while (!start.load()) std::this_thread::yield();
                    
                    for (size_t i = 0; i < OPS_PER_THREAD; i++) {
                        int64_t key = dist(rng);
                        if (i % 3 == 0) tree.insert(key, key);
                        else tree.contains(key);
                    }
                });
            }
            
            auto t0 = std::chrono::high_resolution_clock::now();
            start = true;
            for (auto& w : workers) w.join();
            auto t1 = std::chrono::high_resolution_clock::now();
            
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            size_t total = threads * OPS_PER_THREAD;
            double mops = total / (ms / 1000.0) / 1e6;
            
            std::cout << std::setw(10) << threads << std::setw(12) << shards
                      << std::setw(12) << std::fixed << std::setprecision(2) << mops << " Mops/s"
                      << std::setw(10) << std::setprecision(1) << (tree.get_balance_score() * 100) << "%"
                      << std::setw(12) << tree.get_redirects() << "\n";
            
            results.push_back({"Scale_" + std::to_string(threads) + "t", mops, 
                              tree.get_balance_score(), 0, tree.get_redirects()});
        }
        return results;
    }
    
    // Workload comparison
    std::vector<BenchResult> run_workloads() {
        std::vector<BenchResult> results;
        const int THREADS = 8;
        const int SHARDS = 8;
        
        std::cout << "\n=== WORKLOAD COMPARISON (Original Implementation) ===\n";
        std::cout << std::setw(15) << "Workload" << std::setw(15) << "Throughput"
                  << std::setw(12) << "Balance" << std::setw(12) << "Redirects\n";
        std::cout << std::string(55, '-') << "\n";
        
        std::vector<std::pair<std::string, std::function<std::unique_ptr<WorkloadGenerator>()>>> workloads = {
            {"Uniform", []() { return std::make_unique<UniformWorkload>(); }},
            {"Zipfian", []() { return std::make_unique<ZipfianWorkload>(); }},
            {"Adversarial", [SHARDS]() { return std::make_unique<AdversarialWorkload>(SHARDS); }}
        };
        
        for (auto& [name, factory] : workloads) {
            OriginalParallelAVL<int64_t, int64_t> tree(SHARDS);
            std::atomic<bool> start{false};
            std::vector<std::thread> workers;
            
            for (int t = 0; t < THREADS; t++) {
                workers.emplace_back([&]() {
                    auto wl = factory();
                    while (!start.load()) std::this_thread::yield();
                    for (size_t i = 0; i < OPS_PER_THREAD; i++) {
                        tree.insert(wl->next(), static_cast<int64_t>(i));
                    }
                });
            }
            
            auto t0 = std::chrono::high_resolution_clock::now();
            start = true;
            for (auto& w : workers) w.join();
            auto t1 = std::chrono::high_resolution_clock::now();
            
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            double mops = (THREADS * OPS_PER_THREAD) / (ms / 1000.0) / 1e6;
            
            std::cout << std::setw(15) << name
                      << std::setw(12) << std::fixed << std::setprecision(2) << mops << " Mops/s"
                      << std::setw(10) << std::setprecision(1) << (tree.get_balance_score() * 100) << "%"
                      << std::setw(12) << tree.get_redirects() << "\n";
            
            results.push_back({name, mops, tree.get_balance_score(), 0, tree.get_redirects()});
        }
        return results;
    }
    
    // Latency measurement
    std::vector<BenchResult> run_latency() {
        std::vector<BenchResult> results;
        const size_t SAMPLES = 50000;
        
        std::cout << "\n=== LATENCY DISTRIBUTION (Original Implementation) ===\n";
        std::cout << std::setw(15) << "Workload" << std::setw(12) << "Avg (ns)"
                  << std::setw(12) << "P50 (ns)" << std::setw(12) << "P99 (ns)\n";
        std::cout << std::string(50, '-') << "\n";
        
        std::vector<std::unique_ptr<WorkloadGenerator>> workloads;
        workloads.push_back(std::make_unique<UniformWorkload>());
        workloads.push_back(std::make_unique<ZipfianWorkload>());
        workloads.push_back(std::make_unique<AdversarialWorkload>(8));
        
        for (auto& wl : workloads) {
            OriginalParallelAVL<int64_t, int64_t> tree(8);
            std::vector<double> latencies;
            latencies.reserve(SAMPLES);
            
            // Warmup
            for (size_t i = 0; i < WARMUP_OPS; i++) tree.insert(wl->next(), i);
            
            // Measure
            for (size_t i = 0; i < SAMPLES; i++) {
                auto t0 = std::chrono::high_resolution_clock::now();
                tree.insert(wl->next(), i);
                auto t1 = std::chrono::high_resolution_clock::now();
                latencies.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
            }
            
            std::sort(latencies.begin(), latencies.end());
            double avg = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
            double p50 = latencies[SAMPLES / 2];
            double p99 = latencies[size_t(SAMPLES * 0.99)];
            
            std::cout << std::setw(15) << wl->name()
                      << std::setw(12) << std::fixed << std::setprecision(0) << avg
                      << std::setw(12) << p50
                      << std::setw(12) << p99 << "\n";
            
            results.push_back({wl->name() + "_lat", 0, tree.get_balance_score(), avg, 0});
        }
        return results;
    }
};

int main() {
    std::cout << "╔═══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║  COMPILER COMPARISON - Original ParallelAVL Implementation   ║\n";
    std::cout << "╚═══════════════════════════════════════════════════════════════╝\n";
    
    std::cout << "\nSystem: " << std::thread::hardware_concurrency() << " hardware threads\n";
    
    #if defined(__INTEL_COMPILER) || defined(__INTEL_LLVM_COMPILER)
    std::cout << "Compiler: Intel ICX\n";
    #elif defined(__GNUC__)
    std::cout << "Compiler: GCC " << __GNUC__ << "." << __GNUC_MINOR__ << "\n";
    #elif defined(_MSC_VER)
    std::cout << "Compiler: MSVC\n";
    #endif
    
    CompilerBenchmark bench;
    
    auto scale_results = bench.run_scalability();
    auto workload_results = bench.run_workloads();
    auto latency_results = bench.run_latency();
    
    std::cout << "\n✓ Benchmark complete!\n";
    
    return 0;
}
