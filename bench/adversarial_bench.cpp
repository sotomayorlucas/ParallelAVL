#include "../include/parallel_avl.hpp"
#include <iostream>
#include <vector>
#include <thread>
<parameter name="chrono">
#include <iomanip>
#include <random>
#include <algorithm>

// Attack patterns
enum class AttackType {
    TARGETED_SHARD,      // All keys hash to same shard
    SEQUENTIAL_HOTSPOT,  // Sequential keys creating hotspot
    ZIPFIAN_SKEW,        // Zipfian distribution (80/20 rule)
    RAPID_REDIRECT,      // Rapid consecutive redirects
    MIXED_ADVERSARIAL    // Combination of patterns
};

class AdversarialBenchmark {
private:
    static constexpr size_t NUM_THREADS = 8;
    static constexpr size_t OPS_PER_THREAD = 5000;

    // Zipfian distribution generator
    class ZipfianGenerator {
    private:
        std::mt19937 gen_;
        std::vector<double> probabilities_;
        size_t n_;
        double alpha_;

    public:
        ZipfianGenerator(size_t n, double alpha, unsigned seed)
            : gen_(seed), n_(n), alpha_(alpha) {

            // Compute normalization constant
            double c = 0.0;
            for (size_t i = 1; i <= n; ++i) {
                c += 1.0 / std::pow(i, alpha);
            }

            // Compute probabilities
            probabilities_.resize(n);
            for (size_t i = 0; i < n; ++i) {
                probabilities_[i] = (1.0 / std::pow(i + 1, alpha)) / c;
            }

            // Cumulative probabilities
            for (size_t i = 1; i < n; ++i) {
                probabilities_[i] += probabilities_[i - 1];
            }
        }

        size_t next() {
            std::uniform_real_distribution<double> dist(0.0, 1.0);
            double r = dist(gen_);

            auto it = std::lower_bound(probabilities_.begin(), probabilities_.end(), r);
            return std::distance(probabilities_.begin(), it);
        }
    };

    void run_attack(AttackType attack, const std::string& attack_name) {
        std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
        std::cout << "Attack: " << attack_name << std::endl;
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;

        std::vector<std::pair<std::string, typename ParallelAVL<int, int>::RouterStrategy>> strategies = {
            {"Static Hash", ParallelAVL<int, int>::RouterStrategy::STATIC_HASH},
            {"Load-Aware", ParallelAVL<int, int>::RouterStrategy::LOAD_AWARE},
            {"Virtual Nodes", ParallelAVL<int, int>::RouterStrategy::VIRTUAL_NODES},
            {"Intelligent", ParallelAVL<int, int>::RouterStrategy::INTELLIGENT},
        };

        for (const auto& [strategy_name, strategy] : strategies) {
            ParallelAVL<int, int> tree(8, strategy);

            auto start = std::chrono::high_resolution_clock::now();

            std::vector<std::thread> threads;
            for (size_t tid = 0; tid < NUM_THREADS; ++tid) {
                threads.emplace_back([&, tid]() {
                    switch (attack) {
                        case AttackType::TARGETED_SHARD:
                            targeted_shard_attack(tree, tid);
                            break;
                        case AttackType::SEQUENTIAL_HOTSPOT:
                            sequential_hotspot_attack(tree, tid);
                            break;
                        case AttackType::ZIPFIAN_SKEW:
                            zipfian_skew_attack(tree, tid);
                            break;
                        case AttackType::RAPID_REDIRECT:
                            rapid_redirect_attack(tree, tid);
                            break;
                        case AttackType::MIXED_ADVERSARIAL:
                            mixed_adversarial_attack(tree, tid);
                            break;
                    }
                });
            }

            for (auto& t : threads) {
                t.join();
            }

            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

            auto stats = tree.get_stats();

            std::cout << "\n[" << strategy_name << "]" << std::endl;
            std::cout << "  Time:        " << duration.count() << " ms" << std::endl;
            std::cout << "  Balance:     " << std::fixed << std::setprecision(1)
                     << (stats.balance_score * 100) << "%" << std::endl;
            std::cout << "  Hotspot:     " << (stats.has_hotspot ? "YES ⚠️" : "No") << std::endl;
            std::cout << "  Suspicious:  " << stats.suspicious_patterns << std::endl;
            std::cout << "  Blocked:     " << stats.blocked_redirects << std::endl;
            std::cout << "  Redirects:   " << stats.redirect_index_size << std::endl;

            // Shard distribution
            std::cout << "  Distribution: [";
            for (size_t i = 0; i < stats.shard_sizes.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << stats.shard_sizes[i];
            }
            std::cout << "]" << std::endl;

            // Calculate standard deviation
            double mean = stats.total_size / (double)stats.num_shards;
            double variance = 0;
            for (size_t size : stats.shard_sizes) {
                double diff = size - mean;
                variance += diff * diff;
            }
            double stddev = std::sqrt(variance / stats.num_shards);
            double cv = (mean > 0) ? (stddev / mean) : 0;  // Coefficient of variation

            std::cout << "  Std Dev:     " << std::fixed << std::setprecision(2) << stddev << std::endl;
            std::cout << "  CV:          " << std::fixed << std::setprecision(3) << cv << std::endl;
        }
    }

    // Attack 1: All keys hash to same shard
    template<typename TreeType>
    void targeted_shard_attack(TreeType& tree, size_t thread_id) {
        for (size_t i = 0; i < OPS_PER_THREAD; ++i) {
            int key = thread_id * OPS_PER_THREAD + i;
            key = (key / 8) * 8;  // Force all to shard 0
            tree.insert(key, key);
        }
    }

    // Attack 2: Sequential keys creating natural hotspot
    template<typename TreeType>
    void sequential_hotspot_attack(TreeType& tree, size_t thread_id) {
        // Sequential inserts tend to create imbalanced hash distribution
        for (size_t i = 0; i < OPS_PER_THREAD; ++i) {
            int key = thread_id * OPS_PER_THREAD + i;
            tree.insert(key, key);
        }
    }

    // Attack 3: Zipfian distribution (80% of accesses to 20% of keys)
    template<typename TreeType>
    void zipfian_skew_attack(TreeType& tree, size_t thread_id) {
        ZipfianGenerator zipf(10000, 1.5, thread_id);

        for (size_t i = 0; i < OPS_PER_THREAD; ++i) {
            int key = zipf.next();
            tree.insert(key, key);
        }
    }

    // Attack 4: Rapid consecutive redirects of same key
    template<typename TreeType>
    void rapid_redirect_attack(TreeType& tree, size_t thread_id) {
        std::mt19937 gen(thread_id);
        std::uniform_int_distribution<int> dist(0, 99);

        for (size_t i = 0; i < OPS_PER_THREAD; ++i) {
            // Repeated inserts to force redirect attempts
            int key = dist(gen);
            key = (key / 8) * 8;  // Force to same shard
            tree.insert(key, key);

            // Immediate re-insert (testing cooldown)
            tree.insert(key + 1, key + 1);
        }
    }

    // Attack 5: Mixed adversarial patterns
    template<typename TreeType>
    void mixed_adversarial_attack(TreeType& tree, size_t thread_id) {
        std::mt19937 gen(thread_id);
        std::uniform_int_distribution<int> pattern_dist(0, 2);
        ZipfianGenerator zipf(10000, 1.5, thread_id);

        for (size_t i = 0; i < OPS_PER_THREAD; ++i) {
            int pattern = pattern_dist(gen);
            int key;

            switch (pattern) {
                case 0:  // Targeted
                    key = (i / 8) * 8;
                    break;
                case 1:  // Sequential
                    key = thread_id * OPS_PER_THREAD + i;
                    break;
                case 2:  // Zipfian
                    key = zipf.next();
                    break;
            }

            tree.insert(key, key);
        }
    }

public:
    void run_all() {
        std::cout << "\n╔════════════════════════════════════════════╗" << std::endl;
        std::cout << "║  Adversarial Benchmark Suite               ║" << std::endl;
        std::cout << "╚════════════════════════════════════════════╝" << std::endl;

        run_attack(AttackType::TARGETED_SHARD, "Targeted Shard Attack");
        run_attack(AttackType::SEQUENTIAL_HOTSPOT, "Sequential Hotspot");
        run_attack(AttackType::ZIPFIAN_SKEW, "Zipfian Skew (α=1.5)");
        run_attack(AttackType::RAPID_REDIRECT, "Rapid Redirect Attack");
        run_attack(AttackType::MIXED_ADVERSARIAL, "Mixed Adversarial");

        std::cout << "\n╔════════════════════════════════════════════╗" << std::endl;
        std::cout << "║  Adversarial Testing Complete              ║" << std::endl;
        std::cout << "╚════════════════════════════════════════════╝" << std::endl;

        std::cout << "\nKey Metrics:" << std::endl;
        std::cout << "  Balance:    Higher is better (>70% good)" << std::endl;
        std::cout << "  Suspicious: Lower is better (attack detection)" << std::endl;
        std::cout << "  Blocked:    Higher under attack (defense working)" << std::endl;
        std::cout << "  CV:         Lower is better (<0.3 balanced)" << std::endl;
        std::cout << std::endl;
    }
};

int main() {
    AdversarialBenchmark bench;
    bench.run_all();
    return 0;
}
