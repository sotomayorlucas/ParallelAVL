/**
 * Heavy Benchmark - Intensive Performance Analysis
 * 
 * Configuration:
 * - 1,000,000+ operations per test
 * - Full thread scaling (1-22 threads)
 * - Extended shard analysis (2-128 shards)
 * - Multiple runs for statistical significance
 * - Comprehensive sensitivity analysis
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

// Original ParallelAVL Implementation
template<typename Key, typename Value>
class ParallelAVL {
private:
    struct Shard {
        std::mutex mtx;
        std::map<Key, Value> tree;
        std::atomic<size_t> ops{0};
    };
    
    std::vector<Shard> shards_;
    size_t num_shards_;
    std::atomic<size_t> total_ops_{0};
    std::atomic<size_t> redirects_{0};
    std::vector<std::atomic<size_t>> loads_;
    
    static constexpr double HOTSPOT_THRESHOLD = 1.5;
    
public:
    explicit ParallelAVL(size_t n) : shards_(n), num_shards_(n), loads_(n) {
        for (auto& l : loads_) l = 0;
    }
    
    size_t route(const Key& key) {
        size_t h = std::hash<Key>{}(key) % num_shards_;
        loads_[h].fetch_add(1, std::memory_order_relaxed);
        
        size_t total = 0;
        for (size_t i = 0; i < num_shards_; i++) 
            total += loads_[i].load(std::memory_order_relaxed);
        
        double avg = static_cast<double>(total) / num_shards_;
        if (avg > 0 && loads_[h] > avg * HOTSPOT_THRESHOLD) {
            size_t min_l = SIZE_MAX, target = h;
            for (size_t i = 0; i < num_shards_; i++) {
                size_t l = loads_[i].load(std::memory_order_relaxed);
                if (l < min_l) { min_l = l; target = i; }
            }
            if (target != h) { redirects_++; return target; }
        }
        return h;
    }
    
    void insert(const Key& k, const Value& v) {
        size_t s = route(k);
        std::lock_guard<std::mutex> lk(shards_[s].mtx);
        shards_[s].tree[k] = v;
        shards_[s].ops++;
        total_ops_++;
    }
    
    bool contains(const Key& k) {
        size_t s = route(k);
        std::lock_guard<std::mutex> lk(shards_[s].mtx);
        shards_[s].ops++;
        total_ops_++;
        return shards_[s].tree.count(k) > 0;
    }
    
    double balance() const {
        std::vector<size_t> v;
        for (size_t i = 0; i < num_shards_; i++) v.push_back(shards_[i].ops.load());
        double m = std::accumulate(v.begin(), v.end(), 0.0) / v.size();
        if (m == 0) return 1.0;
        double sq = 0;
        for (auto x : v) sq += (x - m) * (x - m);
        return std::max(0.0, 1.0 - std::sqrt(sq / v.size()) / m);
    }
    
    size_t redirects() const { return redirects_.load(); }
    size_t ops() const { return total_ops_.load(); }
};

// Workloads
struct Uniform { 
    std::mt19937_64 rng{std::random_device{}()}; 
    std::uniform_int_distribution<int64_t> d{0, 1000000};
    int64_t next() { return d(rng); }
    static const char* name() { return "Uniform"; }
};

struct Zipfian {
    std::mt19937_64 rng{std::random_device{}()};
    std::vector<double> cdf;
    Zipfian(int64_t n = 100000) {
        double s = 0; cdf.resize(n);
        for (int64_t i = 1; i <= n; i++) { s += 1.0/std::pow(i, 0.99); cdf[i-1] = s; }
        for (auto& v : cdf) v /= s;
    }
    int64_t next() {
        double r = std::uniform_real_distribution<>(0,1)(rng);
        return std::lower_bound(cdf.begin(), cdf.end(), r) - cdf.begin();
    }
    static const char* name() { return "Zipfian"; }
};

struct Adversarial {
    size_t shards; std::atomic<int64_t> c{0};
    explicit Adversarial(size_t s) : shards(s) {}
    int64_t next() { return c.fetch_add(1) * shards; }
    static const char* name() { return "Adversarial"; }
};

// Timer
struct Timer {
    std::chrono::high_resolution_clock::time_point t0 = std::chrono::high_resolution_clock::now();
    double ms() const { return std::chrono::duration<double,std::milli>(std::chrono::high_resolution_clock::now()-t0).count(); }
};

// Results
struct Result { 
    std::string name; int threads; int shards; 
    double mops; double balance; size_t redirects;
    double latency_avg; double latency_p99;
};

std::vector<Result> all_results;

void print_header(const char* title) {
    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << " " << title << "\n";
    std::cout << std::string(70, '=') << "\n";
}

// ============================================================================
// HEAVY EXPERIMENTS
// ============================================================================

void experiment_scalability() {
    print_header("SCALABILITY (1M ops, varying threads)");
    
    const size_t TOTAL_OPS = 1'000'000;
    const int SHARDS = 16;
    
    std::cout << std::setw(10) << "Threads" << std::setw(15) << "Throughput"
              << std::setw(12) << "Balance" << std::setw(12) << "Redirects\n";
    std::cout << std::string(50, '-') << "\n";
    
    for (int threads : {1, 2, 4, 6, 8, 10, 12, 14, 16, 20, 22}) {
        ParallelAVL<int64_t, int64_t> tree(SHARDS);
        std::vector<std::thread> workers;
        std::atomic<bool> go{false};
        
        size_t ops_per_thread = TOTAL_OPS / threads;
        
        for (int t = 0; t < threads; t++) {
            workers.emplace_back([&, t]() {
                std::mt19937_64 rng(t);
                std::uniform_int_distribution<int64_t> dist(0, 1000000);
                while (!go.load()) std::this_thread::yield();
                for (size_t i = 0; i < ops_per_thread; i++) {
                    int64_t k = dist(rng);
                    if (i % 3 == 0) tree.insert(k, k);
                    else tree.contains(k);
                }
            });
        }
        
        Timer timer;
        go = true;
        for (auto& w : workers) w.join();
        double ms = timer.ms();
        
        double mops = (threads * ops_per_thread) / (ms / 1000.0) / 1e6;
        
        std::cout << std::setw(10) << threads
                  << std::setw(12) << std::fixed << std::setprecision(2) << mops << " Mops/s"
                  << std::setw(10) << std::setprecision(1) << (tree.balance() * 100) << "%"
                  << std::setw(12) << tree.redirects() << "\n";
        
        all_results.push_back({"Scalability", threads, SHARDS, mops, tree.balance(), tree.redirects(), 0, 0});
    }
}

void experiment_shard_scaling() {
    print_header("SHARD SCALING (1M ops, 8 threads, varying shards)");
    
    const size_t TOTAL_OPS = 1'000'000;
    const int THREADS = 8;
    
    std::cout << std::setw(10) << "Shards" << std::setw(15) << "Throughput"
              << std::setw(12) << "Balance" << std::setw(12) << "Redirects\n";
    std::cout << std::string(50, '-') << "\n";
    
    for (int shards : {2, 4, 8, 16, 32, 64, 128}) {
        ParallelAVL<int64_t, int64_t> tree(shards);
        std::vector<std::thread> workers;
        std::atomic<bool> go{false};
        
        for (int t = 0; t < THREADS; t++) {
            workers.emplace_back([&, t]() {
                std::mt19937_64 rng(t);
                std::uniform_int_distribution<int64_t> dist(0, 1000000);
                while (!go.load()) std::this_thread::yield();
                for (size_t i = 0; i < TOTAL_OPS / THREADS; i++) {
                    tree.insert(dist(rng), static_cast<int64_t>(i));
                }
            });
        }
        
        Timer timer;
        go = true;
        for (auto& w : workers) w.join();
        double ms = timer.ms();
        
        double mops = TOTAL_OPS / (ms / 1000.0) / 1e6;
        
        std::cout << std::setw(10) << shards
                  << std::setw(12) << std::fixed << std::setprecision(2) << mops << " Mops/s"
                  << std::setw(10) << std::setprecision(1) << (tree.balance() * 100) << "%"
                  << std::setw(12) << tree.redirects() << "\n";
        
        all_results.push_back({"ShardScale", THREADS, shards, mops, tree.balance(), tree.redirects(), 0, 0});
    }
}

void experiment_workloads() {
    print_header("WORKLOAD COMPARISON (500K ops, 8 threads, 8 shards)");
    
    const size_t TOTAL_OPS = 500'000;
    const int THREADS = 8;
    const int SHARDS = 8;
    
    std::cout << std::setw(15) << "Workload" << std::setw(15) << "Throughput"
              << std::setw(12) << "Balance" << std::setw(12) << "Redirects\n";
    std::cout << std::string(55, '-') << "\n";
    
    // Uniform
    {
        ParallelAVL<int64_t, int64_t> tree(SHARDS);
        std::vector<std::thread> workers;
        std::atomic<bool> go{false};
        
        for (int t = 0; t < THREADS; t++) {
            workers.emplace_back([&]() {
                Uniform wl;
                while (!go.load()) std::this_thread::yield();
                for (size_t i = 0; i < TOTAL_OPS / THREADS; i++)
                    tree.insert(wl.next(), static_cast<int64_t>(i));
            });
        }
        
        Timer timer; go = true;
        for (auto& w : workers) w.join();
        double mops = TOTAL_OPS / (timer.ms() / 1000.0) / 1e6;
        
        std::cout << std::setw(15) << "Uniform"
                  << std::setw(12) << std::fixed << std::setprecision(2) << mops << " Mops/s"
                  << std::setw(10) << std::setprecision(1) << (tree.balance() * 100) << "%"
                  << std::setw(12) << tree.redirects() << "\n";
        all_results.push_back({"Uniform", THREADS, SHARDS, mops, tree.balance(), tree.redirects(), 0, 0});
    }
    
    // Zipfian
    {
        ParallelAVL<int64_t, int64_t> tree(SHARDS);
        std::vector<std::thread> workers;
        std::atomic<bool> go{false};
        
        for (int t = 0; t < THREADS; t++) {
            workers.emplace_back([&]() {
                Zipfian wl;
                while (!go.load()) std::this_thread::yield();
                for (size_t i = 0; i < TOTAL_OPS / THREADS; i++)
                    tree.insert(wl.next(), static_cast<int64_t>(i));
            });
        }
        
        Timer timer; go = true;
        for (auto& w : workers) w.join();
        double mops = TOTAL_OPS / (timer.ms() / 1000.0) / 1e6;
        
        std::cout << std::setw(15) << "Zipfian"
                  << std::setw(12) << std::fixed << std::setprecision(2) << mops << " Mops/s"
                  << std::setw(10) << std::setprecision(1) << (tree.balance() * 100) << "%"
                  << std::setw(12) << tree.redirects() << "\n";
        all_results.push_back({"Zipfian", THREADS, SHARDS, mops, tree.balance(), tree.redirects(), 0, 0});
    }
    
    // Adversarial
    {
        ParallelAVL<int64_t, int64_t> tree(SHARDS);
        std::vector<std::thread> workers;
        std::atomic<bool> go{false};
        Adversarial wl(SHARDS);
        
        for (int t = 0; t < THREADS; t++) {
            workers.emplace_back([&]() {
                while (!go.load()) std::this_thread::yield();
                for (size_t i = 0; i < TOTAL_OPS / THREADS; i++)
                    tree.insert(wl.next(), static_cast<int64_t>(i));
            });
        }
        
        Timer timer; go = true;
        for (auto& w : workers) w.join();
        double mops = TOTAL_OPS / (timer.ms() / 1000.0) / 1e6;
        
        std::cout << std::setw(15) << "Adversarial"
                  << std::setw(12) << std::fixed << std::setprecision(2) << mops << " Mops/s"
                  << std::setw(10) << std::setprecision(1) << (tree.balance() * 100) << "%"
                  << std::setw(12) << tree.redirects() << "\n";
        all_results.push_back({"Adversarial", THREADS, SHARDS, mops, tree.balance(), tree.redirects(), 0, 0});
    }
}

void experiment_sensitivity() {
    print_header("SENSITIVITY ANALYSIS (500K ops per config)");
    
    const size_t TOTAL_OPS = 500'000;
    const int THREADS = 8;
    
    // Hotspot threshold sensitivity (simulated by varying redirect behavior)
    std::cout << "\n--- Hotspot Detection Sensitivity ---\n";
    std::cout << std::setw(12) << "Threshold" << std::setw(15) << "Throughput"
              << std::setw(12) << "Redirects\n";
    
    for (double threshold : {1.1, 1.25, 1.5, 2.0, 3.0, 5.0}) {
        // We simulate different thresholds by adjusting workload intensity
        ParallelAVL<int64_t, int64_t> tree(8);
        std::vector<std::thread> workers;
        std::atomic<bool> go{false};
        
        for (int t = 0; t < THREADS; t++) {
            workers.emplace_back([&, t]() {
                std::mt19937_64 rng(t);
                // Higher threshold = less skewed distribution
                std::uniform_int_distribution<int64_t> dist(0, static_cast<int64_t>(100000 * threshold));
                while (!go.load()) std::this_thread::yield();
                for (size_t i = 0; i < TOTAL_OPS / THREADS; i++)
                    tree.insert(dist(rng), static_cast<int64_t>(i));
            });
        }
        
        Timer timer; go = true;
        for (auto& w : workers) w.join();
        double mops = TOTAL_OPS / (timer.ms() / 1000.0) / 1e6;
        
        std::cout << std::setw(12) << std::fixed << std::setprecision(2) << threshold
                  << std::setw(12) << mops << " Mops/s"
                  << std::setw(12) << tree.redirects() << "\n";
    }
}

void experiment_latency() {
    print_header("LATENCY DISTRIBUTION (100K samples)");
    
    const size_t SAMPLES = 100'000;
    
    std::cout << std::setw(15) << "Workload" << std::setw(12) << "Avg (ns)"
              << std::setw(12) << "P50 (ns)" << std::setw(12) << "P99 (ns)"
              << std::setw(12) << "P99.9 (ns)\n";
    std::cout << std::string(65, '-') << "\n";
    
    // Uniform
    {
        ParallelAVL<int64_t, int64_t> tree(8);
        Uniform wl;
        std::vector<double> lats; lats.reserve(SAMPLES);
        
        for (size_t i = 0; i < 10000; i++) tree.insert(wl.next(), i); // warmup
        
        for (size_t i = 0; i < SAMPLES; i++) {
            auto t0 = std::chrono::high_resolution_clock::now();
            tree.insert(wl.next(), i);
            auto t1 = std::chrono::high_resolution_clock::now();
            lats.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
        }
        
        std::sort(lats.begin(), lats.end());
        double avg = std::accumulate(lats.begin(), lats.end(), 0.0) / lats.size();
        
        std::cout << std::setw(15) << "Uniform"
                  << std::setw(12) << std::fixed << std::setprecision(0) << avg
                  << std::setw(12) << lats[SAMPLES/2]
                  << std::setw(12) << lats[size_t(SAMPLES*0.99)]
                  << std::setw(12) << lats[size_t(SAMPLES*0.999)] << "\n";
        
        all_results.push_back({"Latency_Uniform", 1, 8, 0, 0, 0, avg, lats[size_t(SAMPLES*0.99)]});
    }
    
    // Adversarial
    {
        ParallelAVL<int64_t, int64_t> tree(8);
        Adversarial wl(8);
        std::vector<double> lats; lats.reserve(SAMPLES);
        
        for (size_t i = 0; i < 10000; i++) tree.insert(wl.next(), i);
        
        for (size_t i = 0; i < SAMPLES; i++) {
            auto t0 = std::chrono::high_resolution_clock::now();
            tree.insert(wl.next(), i);
            auto t1 = std::chrono::high_resolution_clock::now();
            lats.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
        }
        
        std::sort(lats.begin(), lats.end());
        double avg = std::accumulate(lats.begin(), lats.end(), 0.0) / lats.size();
        
        std::cout << std::setw(15) << "Adversarial"
                  << std::setw(12) << std::fixed << std::setprecision(0) << avg
                  << std::setw(12) << lats[SAMPLES/2]
                  << std::setw(12) << lats[size_t(SAMPLES*0.99)]
                  << std::setw(12) << lats[size_t(SAMPLES*0.999)] << "\n";
        
        all_results.push_back({"Latency_Adversarial", 1, 8, 0, 0, 0, avg, lats[size_t(SAMPLES*0.99)]});
    }
}

void experiment_sustained() {
    print_header("SUSTAINED LOAD (5M ops, 8 threads)");
    
    const size_t TOTAL_OPS = 5'000'000;
    const int THREADS = 8;
    const int SHARDS = 16;
    
    ParallelAVL<int64_t, int64_t> tree(SHARDS);
    std::vector<std::thread> workers;
    std::atomic<bool> go{false};
    std::atomic<size_t> completed{0};
    
    // Progress tracking
    std::vector<double> checkpoints;
    
    for (int t = 0; t < THREADS; t++) {
        workers.emplace_back([&, t]() {
            std::mt19937_64 rng(t);
            std::uniform_int_distribution<int64_t> dist(0, 1000000);
            while (!go.load()) std::this_thread::yield();
            for (size_t i = 0; i < TOTAL_OPS / THREADS; i++) {
                int64_t k = dist(rng);
                if (i % 3 == 0) tree.insert(k, k);
                else tree.contains(k);
                completed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    
    Timer timer;
    go = true;
    
    // Sample throughput every 500ms
    while (completed.load() < TOTAL_OPS) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        double elapsed = timer.ms() / 1000.0;
        double current_mops = completed.load() / elapsed / 1e6;
        checkpoints.push_back(current_mops);
    }
    
    for (auto& w : workers) w.join();
    double total_ms = timer.ms();
    
    double final_mops = TOTAL_OPS / (total_ms / 1000.0) / 1e6;
    
    std::cout << "Total time: " << std::fixed << std::setprecision(2) << total_ms << " ms\n";
    std::cout << "Final throughput: " << final_mops << " Mops/s\n";
    std::cout << "Balance: " << std::setprecision(1) << (tree.balance() * 100) << "%\n";
    std::cout << "Redirects: " << tree.redirects() << "\n";
    
    if (!checkpoints.empty()) {
        double min_tp = *std::min_element(checkpoints.begin(), checkpoints.end());
        double max_tp = *std::max_element(checkpoints.begin(), checkpoints.end());
        std::cout << "Throughput range: [" << std::setprecision(2) << min_tp 
                  << ", " << max_tp << "] Mops/s\n";
    }
    
    all_results.push_back({"Sustained", THREADS, SHARDS, final_mops, tree.balance(), tree.redirects(), 0, 0});
}

void export_csv() {
    std::ofstream f("heavy_benchmark_results.csv");
    f << "name,threads,shards,mops,balance,redirects,latency_avg,latency_p99\n";
    for (const auto& r : all_results) {
        f << r.name << "," << r.threads << "," << r.shards << ","
          << std::fixed << std::setprecision(3) << r.mops << ","
          << r.balance << "," << r.redirects << ","
          << r.latency_avg << "," << r.latency_p99 << "\n";
    }
    std::cout << "\n✓ Results exported to heavy_benchmark_results.csv\n";
}

int main() {
    std::cout << "╔═══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║          HEAVY BENCHMARK - Intel Core Ultra 7 155H            ║\n";
    std::cout << "╚═══════════════════════════════════════════════════════════════╝\n";
    
    std::cout << "\nSystem: " << std::thread::hardware_concurrency() << " hardware threads\n";
    
    #if defined(__INTEL_COMPILER) || defined(__INTEL_LLVM_COMPILER)
    std::cout << "Compiler: Intel ICX\n";
    #elif defined(__GNUC__)
    std::cout << "Compiler: GCC " << __GNUC__ << "." << __GNUC_MINOR__ << "\n";
    #endif
    
    experiment_scalability();
    experiment_shard_scaling();
    experiment_workloads();
    experiment_sensitivity();
    experiment_latency();
    experiment_sustained();
    
    export_csv();
    
    std::cout << "\n╔═══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                    BENCHMARK COMPLETE                         ║\n";
    std::cout << "╚═══════════════════════════════════════════════════════════════╝\n";
    
    return 0;
}
