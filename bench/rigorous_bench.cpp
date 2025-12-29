#include "../include/parallel_avl.hpp"
#include "../include/workloads.hpp"
#include <iostream>
#include <iomanip>
#include <vector>
#include <thread>
#include <chrono>
#include <cmath>
#include <numeric>
#include <algorithm>

// Rigorous Statistical Benchmarking
//
// Especificación:
// - Multiple runs (10+) para estadísticas confiables
// - Mean, stddev, 95% confidence intervals
// - Latency percentiles (P50, P90, P99, P99.9)
// - Balance score tracking
// - Lock contention ratio

struct BenchConfig {
    size_t num_operations = 1'000'000;
    size_t num_threads = std::thread::hardware_concurrency();
    size_t warmup_ops = 100'000;
    size_t num_runs = 10;
    WorkloadType workload = WorkloadType::UNIFORM;
    size_t key_space = 100'000;
    size_t num_shards = 8;
};

struct LatencyStats {
    std::vector<double> samples;  // microseconds

    void record(double latency_us) {
        samples.push_back(latency_us);
    }

    struct Percentiles {
        double p50;
        double p90;
        double p99;
        double p999;
        double mean;
        double min;
        double max;
    };

    Percentiles compute() {
        if (samples.empty()) {
            return {0, 0, 0, 0, 0, 0, 0};
        }

        std::sort(samples.begin(), samples.end());

        auto percentile = [&](double p) {
            size_t idx = static_cast<size_t>(samples.size() * p);
            if (idx >= samples.size()) idx = samples.size() - 1;
            return samples[idx];
        };

        double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
        double mean = sum / samples.size();

        return {
            .p50 = percentile(0.50),
            .p90 = percentile(0.90),
            .p99 = percentile(0.99),
            .p999 = percentile(0.999),
            .mean = mean,
            .min = samples.front(),
            .max = samples.back()
        };
    }
};

struct SingleRunResult {
    double throughput_ops_per_sec;
    LatencyStats latencies;
    double balance_score;
    size_t redirect_count;
};

struct AggregatedResult {
    // Throughput statistics
    double mean_throughput;
    double stddev_throughput;
    double ci_95_lower;
    double ci_95_upper;

    // Latency statistics (aggregated from all runs)
    LatencyStats::Percentiles latencies;

    // Other metrics
    double mean_balance;
    double mean_redirects;
};

class RigorousBenchmark {
private:
    BenchConfig config_;

    // Compute statistics
    static double mean(const std::vector<double>& values) {
        if (values.empty()) return 0.0;
        return std::accumulate(values.begin(), values.end(), 0.0) / values.size();
    }

    static double stddev(const std::vector<double>& values, double mean_val) {
        if (values.size() < 2) return 0.0;

        double variance = 0.0;
        for (double v : values) {
            double diff = v - mean_val;
            variance += diff * diff;
        }
        variance /= (values.size() - 1);  // Sample variance

        return std::sqrt(variance);
    }

    // 95% confidence interval using t-distribution
    // Para n >= 30, aproximamos con normal (z = 1.96)
    static std::pair<double, double> confidence_interval_95(const std::vector<double>& values, double mean_val, double stddev_val) {
        if (values.size() < 2) return {mean_val, mean_val};

        double z = 1.96;  // 95% CI for normal distribution
        double margin = z * stddev_val / std::sqrt(values.size());

        return {mean_val - margin, mean_val + margin};
    }

    SingleRunResult run_single(typename ParallelAVL<int, int>::RouterStrategy strategy) {
        ParallelAVL<int, int> tree(config_.num_shards, strategy);
        LatencyStats latencies;

        auto workload = WorkloadFactory<int>::create(
            config_.workload,
            config_.key_space,
            config_.num_shards
        );

        // Warmup
        for (size_t i = 0; i < config_.warmup_ops; ++i) {
            int key = workload->next();
            tree.insert(key, key * 2);
        }

        workload->reset();

        // Actual benchmark
        std::atomic<size_t> completed_ops{0};
        std::vector<std::thread> threads;

        auto start_time = std::chrono::high_resolution_clock::now();

        for (size_t tid = 0; tid < config_.num_threads; ++tid) {
            threads.emplace_back([&, tid]() {
                auto local_workload = WorkloadFactory<int>::create(
                    config_.workload,
                    config_.key_space,
                    config_.num_shards,
                    tid
                );

                size_t ops_per_thread = config_.num_operations / config_.num_threads;

                for (size_t i = 0; i < ops_per_thread; ++i) {
                    int key = local_workload->next();

                    auto op_start = std::chrono::high_resolution_clock::now();
                    tree.insert(key, key * 2);
                    auto op_end = std::chrono::high_resolution_clock::now();

                    double latency_us = std::chrono::duration<double, std::micro>(op_end - op_start).count();
                    latencies.record(latency_us);

                    completed_ops.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        double duration_sec = std::chrono::duration<double>(end_time - start_time).count();

        double throughput = completed_ops.load() / duration_sec;

        auto stats = tree.get_stats();

        return {
            .throughput_ops_per_sec = throughput,
            .latencies = std::move(latencies),
            .balance_score = stats.balance_score,
            .redirect_count = stats.redirect_index_size
        };
    }

public:
    explicit RigorousBenchmark(const BenchConfig& config) : config_(config) {}

    AggregatedResult run(typename ParallelAVL<int, int>::RouterStrategy strategy) {
        std::vector<double> throughputs;
        std::vector<double> balances;
        std::vector<double> redirects;
        LatencyStats all_latencies;

        std::cout << "Running " << config_.num_runs << " iterations..." << std::flush;

        for (size_t run = 0; run < config_.num_runs; ++run) {
            auto result = run_single(strategy);

            throughputs.push_back(result.throughput_ops_per_sec);
            balances.push_back(result.balance_score);
            redirects.push_back(result.redirect_count);

            // Aggregate latencies
            for (double lat : result.latencies.samples) {
                all_latencies.record(lat);
            }

            std::cout << "." << std::flush;
        }

        std::cout << " done!" << std::endl;

        // Compute statistics
        double mean_tput = mean(throughputs);
        double stddev_tput = stddev(throughputs, mean_tput);
        auto [ci_lower, ci_upper] = confidence_interval_95(throughputs, mean_tput, stddev_tput);

        return {
            .mean_throughput = mean_tput,
            .stddev_throughput = stddev_tput,
            .ci_95_lower = ci_lower,
            .ci_95_upper = ci_upper,
            .latencies = all_latencies.compute(),
            .mean_balance = mean(balances),
            .mean_redirects = mean(redirects)
        };
    }

    void print_result(const std::string& strategy_name, const AggregatedResult& result) {
        std::cout << "\n━━━ " << strategy_name << " ━━━" << std::endl;

        // Throughput
        std::cout << "Throughput (ops/sec):" << std::endl;
        std::cout << "  Mean:    " << std::fixed << std::setprecision(2)
                  << (result.mean_throughput / 1e6) << " M" << std::endl;
        std::cout << "  Stddev:  " << std::fixed << std::setprecision(2)
                  << (result.stddev_throughput / 1e6) << " M" << std::endl;
        std::cout << "  95% CI:  [" << std::fixed << std::setprecision(2)
                  << (result.ci_95_lower / 1e6) << ", "
                  << (result.ci_95_upper / 1e6) << "] M" << std::endl;

        // Latency
        std::cout << "\nLatency (μs):" << std::endl;
        std::cout << "  Mean:    " << std::fixed << std::setprecision(2) << result.latencies.mean << std::endl;
        std::cout << "  P50:     " << std::fixed << std::setprecision(2) << result.latencies.p50 << std::endl;
        std::cout << "  P90:     " << std::fixed << std::setprecision(2) << result.latencies.p90 << std::endl;
        std::cout << "  P99:     " << std::fixed << std::setprecision(2) << result.latencies.p99 << std::endl;
        std::cout << "  P99.9:   " << std::fixed << std::setprecision(2) << result.latencies.p999 << std::endl;

        // Other metrics
        std::cout << "\nOther Metrics:" << std::endl;
        std::cout << "  Balance: " << std::fixed << std::setprecision(1)
                  << (result.mean_balance * 100) << "%" << std::endl;
        std::cout << "  Redirects: " << std::fixed << std::setprecision(0)
                  << result.mean_redirects << std::endl;
    }

    void run_all_strategies() {
        std::cout << "\n╔════════════════════════════════════════════╗" << std::endl;
        std::cout << "║  Rigorous Statistical Benchmark            ║" << std::endl;
        std::cout << "╚════════════════════════════════════════════╝" << std::endl;

        std::cout << "\nConfiguration:" << std::endl;
        std::cout << "  Operations: " << config_.num_operations << std::endl;
        std::cout << "  Threads:    " << config_.num_threads << std::endl;
        std::cout << "  Shards:     " << config_.num_shards << std::endl;
        std::cout << "  Warmup:     " << config_.warmup_ops << std::endl;
        std::cout << "  Runs:       " << config_.num_runs << std::endl;
        std::cout << "  Workload:   " << WorkloadFactory<int>::name(config_.workload) << std::endl;

        std::vector<std::pair<std::string, typename ParallelAVL<int, int>::RouterStrategy>> strategies = {
            {"Static Hash", ParallelAVL<int, int>::RouterStrategy::STATIC_HASH},
            {"Load-Aware", ParallelAVL<int, int>::RouterStrategy::LOAD_AWARE},
            {"Consistent Hash", ParallelAVL<int, int>::RouterStrategy::CONSISTENT_HASH},
            {"Intelligent", ParallelAVL<int, int>::RouterStrategy::INTELLIGENT},
        };

        for (const auto& [name, strategy] : strategies) {
            auto result = run(strategy);
            print_result(name, result);
        }

        std::cout << "\n╔════════════════════════════════════════════╗" << std::endl;
        std::cout << "║  Benchmark Complete                        ║" << std::endl;
        std::cout << "╚════════════════════════════════════════════╝\n" << std::endl;
    }
};

int main(int argc, char** argv) {
    BenchConfig config;

    // Parse command line args (simple)
    if (argc > 1) {
        config.num_operations = std::stoull(argv[1]);
    }
    if (argc > 2) {
        config.num_threads = std::stoull(argv[2]);
    }

    std::cout << "=== Parallel AVL Benchmark ===" << std::endl;
    std::cout << "Hardware: " << std::thread::hardware_concurrency() << " cores" << std::endl;

    // Run different workloads
    std::vector<WorkloadType> workloads = {
        WorkloadType::UNIFORM,
        WorkloadType::ZIPFIAN,
        WorkloadType::SEQUENTIAL,
        WorkloadType::ADVERSARIAL
    };

    for (auto workload : workloads) {
        config.workload = workload;

        std::cout << "\n" << std::string(50, '=') << std::endl;
        std::cout << "Workload: " << WorkloadFactory<int>::name(workload) << std::endl;
        std::cout << std::string(50, '=') << std::endl;

        RigorousBenchmark bench(config);
        bench.run_all_strategies();
    }

    return 0;
}
