#include "../include/parallel_avl.hpp"
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <algorithm>
#include <iomanip>
#include <random>
#include <atomic>

// Latency histogram with percentiles
class LatencyHistogram {
private:
    std::vector<double> latencies_;  // microseconds
    std::mutex mutex_;

public:
    void record(double latency_us) {
        std::lock_guard<std::mutex> lock(mutex_);
        latencies_.push_back(latency_us);
    }

    struct Statistics {
        double min;
        double max;
        double mean;
        double median;
        double p50;
        double p90;
        double p95;
        double p99;
        double p999;
        size_t count;
    };

    Statistics compute() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (latencies_.empty()) {
            return {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        }

        std::sort(latencies_.begin(), latencies_.end());

        Statistics stats;
        stats.count = latencies_.size();
        stats.min = latencies_.front();
        stats.max = latencies_.back();

        // Mean
        double sum = 0;
        for (double lat : latencies_) {
            sum += lat;
        }
        stats.mean = sum / latencies_.size();

        // Percentiles
        auto percentile = [&](double p) {
            size_t idx = static_cast<size_t>(latencies_.size() * p);
            if (idx >= latencies_.size()) idx = latencies_.size() - 1;
            return latencies_[idx];
        };

        stats.median = percentile(0.50);
        stats.p50 = percentile(0.50);
        stats.p90 = percentile(0.90);
        stats.p95 = percentile(0.95);
        stats.p99 = percentile(0.99);
        stats.p999 = percentile(0.999);

        return stats;
    }

    void print(const std::string& operation) const {
        auto stats = const_cast<LatencyHistogram*>(this)->compute();

        std::cout << "\n" << operation << " Latency (μs):" << std::endl;
        std::cout << "  Count:  " << stats.count << std::endl;
        std::cout << "  Min:    " << std::fixed << std::setprecision(2) << stats.min << std::endl;
        std::cout << "  Mean:   " << std::fixed << std::setprecision(2) << stats.mean << std::endl;
        std::cout << "  Median: " << std::fixed << std::setprecision(2) << stats.median << std::endl;
        std::cout << "  P90:    " << std::fixed << std::setprecision(2) << stats.p90 << std::endl;
        std::cout << "  P95:    " << std::fixed << std::setprecision(2) << stats.p95 << std::endl;
        std::cout << "  P99:    " << std::fixed << std::setprecision(2) << stats.p99 << std::endl;
        std::cout << "  P99.9:  " << std::fixed << std::setprecision(2) << stats.p999 << std::endl;
        std::cout << "  Max:    " << std::fixed << std::setprecision(2) << stats.max << std::endl;
    }
};

// Benchmark configuration
struct BenchConfig {
    size_t num_threads;
    size_t ops_per_thread;
    double read_ratio;  // 0.0 = all writes, 1.0 = all reads
    std::string workload_name;
};

class LatencyBenchmark {
private:
    static constexpr size_t KEY_SPACE = 100000;

    template<typename TreeType>
    void run_benchmark(TreeType& tree, const BenchConfig& config,
                      LatencyHistogram& insert_hist,
                      LatencyHistogram& contains_hist,
                      LatencyHistogram& get_hist) {

        std::vector<std::thread> threads;
        std::atomic<bool> start_flag{false};

        // Pre-populate tree
        for (size_t i = 0; i < KEY_SPACE / 2; ++i) {
            tree.insert(i, i);
        }

        for (size_t tid = 0; tid < config.num_threads; ++tid) {
            threads.emplace_back([&, tid]() {
                std::mt19937 gen(tid);
                std::uniform_int_distribution<int> key_dist(0, KEY_SPACE - 1);
                std::uniform_real_distribution<double> op_dist(0.0, 1.0);

                // Wait for start signal
                while (!start_flag.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }

                for (size_t i = 0; i < config.ops_per_thread; ++i) {
                    int key = key_dist(gen);
                    double op_type = op_dist(gen);

                    auto start = std::chrono::high_resolution_clock::now();

                    if (op_type < config.read_ratio) {
                        // Read operations
                        if (op_type < config.read_ratio / 2) {
                            tree.contains(key);
                            auto end = std::chrono::high_resolution_clock::now();
                            double latency = std::chrono::duration<double, std::micro>(end - start).count();
                            contains_hist.record(latency);
                        } else {
                            tree.get(key);
                            auto end = std::chrono::high_resolution_clock::now();
                            double latency = std::chrono::duration<double, std::micro>(end - start).count();
                            get_hist.record(latency);
                        }
                    } else {
                        // Write operation (insert)
                        tree.insert(key, key * 2);
                        auto end = std::chrono::high_resolution_clock::now();
                        double latency = std::chrono::duration<double, std::micro>(end - start).count();
                        insert_hist.record(latency);
                    }
                }
            });
        }

        // Start benchmark
        start_flag.store(true, std::memory_order_release);

        // Wait for completion
        for (auto& t : threads) {
            t.join();
        }
    }

public:
    void run_all() {
        std::cout << "\n╔════════════════════════════════════════════╗" << std::endl;
        std::cout << "║  Latency Benchmark Suite                   ║" << std::endl;
        std::cout << "╚════════════════════════════════════════════╝" << std::endl;

        std::vector<BenchConfig> configs = {
            {8, 10000, 0.95, "Read-Heavy (95% reads)"},
            {8, 10000, 0.50, "Mixed (50/50)"},
            {8, 10000, 0.10, "Write-Heavy (90% writes)"},
        };

        for (const auto& config : configs) {
            std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
            std::cout << "Workload: " << config.workload_name << std::endl;
            std::cout << "Threads: " << config.num_threads << std::endl;
            std::cout << "Ops/thread: " << config.ops_per_thread << std::endl;
            std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;

            // Test each routing strategy
            std::vector<std::pair<std::string, typename ParallelAVL<int, int>::RouterStrategy>> strategies = {
                {"Static Hash", ParallelAVL<int, int>::RouterStrategy::STATIC_HASH},
                {"Load-Aware", ParallelAVL<int, int>::RouterStrategy::LOAD_AWARE},
                {"Virtual Nodes", ParallelAVL<int, int>::RouterStrategy::VIRTUAL_NODES},
                {"Intelligent", ParallelAVL<int, int>::RouterStrategy::INTELLIGENT},
            };

            for (const auto& [strategy_name, strategy] : strategies) {
                std::cout << "\n--- Strategy: " << strategy_name << " ---" << std::endl;

                ParallelAVL<int, int> tree(8, strategy);
                LatencyHistogram insert_hist, contains_hist, get_hist;

                auto start = std::chrono::high_resolution_clock::now();
                run_benchmark(tree, config, insert_hist, contains_hist, get_hist);
                auto end = std::chrono::high_resolution_clock::now();

                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
                size_t total_ops = config.num_threads * config.ops_per_thread;
                double throughput = (total_ops * 1000.0) / duration.count();

                std::cout << "\nThroughput: " << std::fixed << std::setprecision(0)
                         << throughput << " ops/sec" << std::endl;

                insert_hist.print("INSERT");
                contains_hist.print("CONTAINS");
                get_hist.print("GET");

                auto stats = tree.get_stats();
                std::cout << "\nTree Statistics:" << std::endl;
                std::cout << "  Balance: " << (stats.balance_score * 100) << "%" << std::endl;
                std::cout << "  Redirect hit rate: " << stats.redirect_hit_rate << "%" << std::endl;
            }
        }

        std::cout << "\n╔════════════════════════════════════════════╗" << std::endl;
        std::cout << "║  Benchmark Complete                        ║" << std::endl;
        std::cout << "╚════════════════════════════════════════════╝\n" << std::endl;
    }
};

int main() {
    LatencyBenchmark bench;
    bench.run_all();
    return 0;
}
