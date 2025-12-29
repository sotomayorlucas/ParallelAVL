#include "../include/parallel_avl.hpp"
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <cassert>
#include <chrono>
#include <random>

// Test harness
class LinearizabilityTest {
private:
    static constexpr size_t NUM_THREADS = 8;
    static constexpr size_t OPS_PER_THREAD = 1000;

    template<typename Func>
    void run_concurrent(const std::string& test_name, Func func) {
        std::cout << "\n[TEST] " << test_name << std::endl;

        std::vector<std::thread> threads;
        threads.reserve(NUM_THREADS);

        auto start = std::chrono::high_resolution_clock::now();

        for (size_t i = 0; i < NUM_THREADS; ++i) {
            threads.emplace_back(func, i);
        }

        for (auto& t : threads) {
            t.join();
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        std::cout << "  ✓ Completed in " << duration.count() << "ms" << std::endl;
    }

public:
    // Test 1: Basic linearizability - insert then contains
    void test_insert_then_contains() {
        ParallelAVL<int, int> tree(8, ParallelAVL<int, int>::RouterStrategy::INTELLIGENT);

        std::atomic<size_t> failures{0};

        run_concurrent("Insert-Then-Contains", [&](size_t thread_id) {
            std::mt19937 gen(thread_id);
            std::uniform_int_distribution<int> dist(0, 9999);

            for (size_t i = 0; i < OPS_PER_THREAD; ++i) {
                int key = dist(gen);
                int value = thread_id * 10000 + i;

                // Insert
                tree.insert(key, value);

                // Immediately check contains - MUST be true (linearizability)
                if (!tree.contains(key)) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });

        if (failures.load() == 0) {
            std::cout << "  ✓ Linearizability preserved: all inserts immediately visible" << std::endl;
        } else {
            std::cout << "  ✗ FAILED: " << failures.load() << " linearizability violations!" << std::endl;
        }

        assert(failures.load() == 0 && "Linearizability violation detected!");
    }

    // Test 2: Redirected keys are findable
    void test_redirected_keys() {
        ParallelAVL<int, int> tree(8, ParallelAVL<int, int>::RouterStrategy::INTELLIGENT);

        std::cout << "\n[TEST] Redirected Keys Findability" << std::endl;

        // Create hotspot to force redirections
        std::atomic<size_t> redirected_inserts{0};
        std::vector<int> hotspot_keys;

        // Insert keys that hash to same shard (creating hotspot)
        for (int i = 0; i < 100; ++i) {
            int key = i * 8;  // All hash to shard 0
            tree.insert(key, i);
            hotspot_keys.push_back(key);
        }

        auto stats = tree.get_stats();
        std::cout << "  Redirect index size: " << stats.redirect_index_size << std::endl;

        // Verify ALL keys are findable
        size_t not_found = 0;
        for (int key : hotspot_keys) {
            if (!tree.contains(key)) {
                not_found++;
            }
        }

        if (not_found == 0) {
            std::cout << "  ✓ All redirected keys findable" << std::endl;
        } else {
            std::cout << "  ✗ FAILED: " << not_found << " keys not found!" << std::endl;
        }

        assert(not_found == 0 && "Redirected keys not findable!");
    }

    // Test 3: Concurrent insert + search
    void test_concurrent_insert_search() {
        ParallelAVL<int, int> tree(8, ParallelAVL<int, int>::RouterStrategy::INTELLIGENT);

        std::atomic<size_t> insert_failures{0};
        std::atomic<size_t> search_failures{0};

        run_concurrent("Concurrent Insert+Search", [&](size_t thread_id) {
            std::mt19937 gen(thread_id);
            std::uniform_int_distribution<int> dist(0, 9999);

            for (size_t i = 0; i < OPS_PER_THREAD; ++i) {
                if (thread_id % 2 == 0) {
                    // Even threads: insert
                    int key = dist(gen);
                    tree.insert(key, key * 2);

                    // Verify immediately
                    if (!tree.contains(key)) {
                        insert_failures.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    // Odd threads: search
                    int key = dist(gen);
                    auto result = tree.get(key);

                    // If found, value must be correct
                    if (result.has_value() && *result != key * 2) {
                        search_failures.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });

        std::cout << "  Insert failures: " << insert_failures.load() << std::endl;
        std::cout << "  Search failures: " << search_failures.load() << std::endl;

        assert(insert_failures.load() == 0 && "Insert linearizability violation!");
        assert(search_failures.load() == 0 && "Search consistency violation!");
    }

    // Test 4: Remove updates redirect index
    void test_remove_redirect_index() {
        ParallelAVL<int, int> tree(8, ParallelAVL<int, int>::RouterStrategy::INTELLIGENT);

        std::cout << "\n[TEST] Remove Updates Redirect Index" << std::endl;

        // Insert keys that will be redirected
        for (int i = 0; i < 50; ++i) {
            tree.insert(i * 8, i);  // Hash to shard 0, likely redirected
        }

        auto stats_before = tree.get_stats();
        size_t redirects_before = stats_before.redirect_index_size;
        std::cout << "  Redirects before remove: " << redirects_before << std::endl;

        // Remove half
        for (int i = 0; i < 25; ++i) {
            tree.remove(i * 8);
        }

        auto stats_after = tree.get_stats();
        size_t redirects_after = stats_after.redirect_index_size;
        std::cout << "  Redirects after remove: " << redirects_after << std::endl;

        // Verify remaining keys still findable
        size_t not_found = 0;
        for (int i = 25; i < 50; ++i) {
            if (!tree.contains(i * 8)) {
                not_found++;
            }
        }

        std::cout << "  Remaining keys not found: " << not_found << std::endl;

        assert(redirects_after <= redirects_before && "Redirect index not cleaned up!");
        assert(not_found == 0 && "Keys lost after remove!");

        std::cout << "  ✓ Redirect index correctly updated on remove" << std::endl;
    }

    // Test 5: Stress test - many redirections
    void test_redirect_index_stress() {
        ParallelAVL<int, int> tree(8, ParallelAVL<int, int>::RouterStrategy::INTELLIGENT);

        std::atomic<size_t> total_not_found{0};

        run_concurrent("Redirect Index Stress", [&](size_t thread_id) {
            // All threads insert to same shard space (hotspot)
            for (size_t i = 0; i < OPS_PER_THREAD; ++i) {
                int key = thread_id * OPS_PER_THREAD + i;
                key = (key / 8) * 8;  // Force same shard

                tree.insert(key, key);

                // Verify findable
                if (!tree.contains(key)) {
                    total_not_found.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });

        auto stats = tree.get_stats();
        std::cout << "  Total redirects: " << stats.redirect_index_size << std::endl;
        std::cout << "  Redirect hit rate: " << stats.redirect_hit_rate << "%" << std::endl;
        std::cout << "  Keys not found: " << total_not_found.load() << std::endl;

        assert(total_not_found.load() == 0 && "Keys lost under stress!");
        std::cout << "  ✓ Redirect index handles stress correctly" << std::endl;
    }

    // Test 6: Range query correctness with redirections
    void test_range_query_with_redirects() {
        ParallelAVL<int, int> tree(8, ParallelAVL<int, int>::RouterStrategy::INTELLIGENT);

        std::cout << "\n[TEST] Range Query With Redirects" << std::endl;

        // Insert keys
        std::vector<int> inserted_keys;
        for (int i = 0; i < 100; ++i) {
            tree.insert(i, i * 10);
            inserted_keys.push_back(i);
        }

        // Range query
        std::vector<std::pair<int, int>> results;
        tree.range_query(25, 75, std::back_inserter(results));

        std::cout << "  Range [25, 75] returned " << results.size() << " results" << std::endl;

        // Verify all expected keys in range
        size_t expected = 0;
        for (int key : inserted_keys) {
            if (key >= 25 && key <= 75) {
                expected++;
            }
        }

        std::cout << "  Expected: " << expected << ", Got: " << results.size() << std::endl;

        // Verify sorted
        bool sorted = true;
        for (size_t i = 1; i < results.size(); ++i) {
            if (results[i].first < results[i-1].first) {
                sorted = false;
                break;
            }
        }

        assert(results.size() == expected && "Range query missed keys!");
        assert(sorted && "Range query results not sorted!");

        std::cout << "  ✓ Range query correct with redirects" << std::endl;
    }

    // Test 7: Adversary resistance
    void test_adversary_resistance() {
        ParallelAVL<int, int> tree(8, ParallelAVL<int, int>::RouterStrategy::INTELLIGENT);

        std::cout << "\n[TEST] Adversary Resistance" << std::endl;

        // Targeted attack: all keys to shard 0
        for (int i = 0; i < 1000; ++i) {
            tree.insert(i * 8, i);
        }

        auto stats = tree.get_stats();

        std::cout << "  Balance score: " << (stats.balance_score * 100) << "%" << std::endl;
        std::cout << "  Hotspot detected: " << (stats.has_hotspot ? "Yes" : "No") << std::endl;
        std::cout << "  Suspicious patterns: " << stats.suspicious_patterns << std::endl;
        std::cout << "  Blocked redirects: " << stats.blocked_redirects << std::endl;

        // Should maintain reasonable balance despite attack
        assert(stats.balance_score > 0.5 && "Failed to resist targeted attack!");

        std::cout << "  ✓ Successfully resisted targeted attack" << std::endl;
    }

    void run_all() {
        std::cout << "\n╔════════════════════════════════════════════╗" << std::endl;
        std::cout << "║  Linearizability Test Suite               ║" << std::endl;
        std::cout << "╚════════════════════════════════════════════╝" << std::endl;

        test_insert_then_contains();
        test_redirected_keys();
        test_concurrent_insert_search();
        test_remove_redirect_index();
        test_redirect_index_stress();
        test_range_query_with_redirects();
        test_adversary_resistance();

        std::cout << "\n╔════════════════════════════════════════════╗" << std::endl;
        std::cout << "║  ✓ ALL TESTS PASSED                        ║" << std::endl;
        std::cout << "╚════════════════════════════════════════════╝\n" << std::endl;
    }
};

int main() {
    LinearizabilityTest test;
    test.run_all();
    return 0;
}
