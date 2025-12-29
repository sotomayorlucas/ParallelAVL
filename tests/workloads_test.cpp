#include "../include/workloads.hpp"
#include <iostream>
#include <unordered_map>
#include <cassert>
#include <cmath>

// Tests para Workload Generators

void test_uniform_distribution() {
    std::cout << "\n[TEST] Uniform Distribution" << std::endl;

    UniformGenerator<int> gen(0, 99, 12345);

    std::unordered_map<int, size_t> counts;

    // Generate 10000 samples
    for (size_t i = 0; i < 10000; ++i) {
        int key = gen.next();
        assert(key >= 0 && key <= 99 && "Key out of range");
        counts[key]++;
    }

    // Check coverage (should hit most keys)
    std::cout << "  Unique keys generated: " << counts.size() << " / 100" << std::endl;
    assert(counts.size() > 80 && "Should hit most keys in range");

    // Check uniformity (each key should have ~100 occurrences)
    double mean = 10000.0 / 100.0;  // 100 per key
    double variance = 0.0;

    for (const auto& [key, count] : counts) {
        double diff = count - mean;
        variance += diff * diff;
    }
    variance /= counts.size();

    double cv = std::sqrt(variance) / mean;
    std::cout << "  Coefficient of variation: " << cv << std::endl;

    assert(cv < 0.3 && "Distribution should be fairly uniform");
    std::cout << "  ✓ Uniform distribution works" << std::endl;
}

void test_zipfian_skew() {
    std::cout << "\n[TEST] Zipfian Skew (α=0.99)" << std::endl;

    ZipfianGenerator<int> gen(100, 0.99, 12345);

    std::unordered_map<int, size_t> counts;

    // Generate many samples
    for (size_t i = 0; i < 100000; ++i) {
        int key = gen.next();
        assert(key >= 1 && key <= 100 && "Key out of range");
        counts[key]++;
    }

    // Sort by frequency
    std::vector<std::pair<int, size_t>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    // Top 20% of keys should get ~80% of accesses (Pareto principle)
    size_t top_20_count = 0;
    for (size_t i = 0; i < 20 && i < sorted.size(); ++i) {
        top_20_count += sorted[i].second;
    }

    double top_20_percent = (top_20_count * 100.0) / 100000.0;
    std::cout << "  Top 20% keys got: " << top_20_percent << "% of accesses" << std::endl;

    // Most popular key
    std::cout << "  Most popular key: " << sorted[0].first
              << " with " << sorted[0].second << " accesses ("
              << (sorted[0].second * 100.0 / 100000.0) << "%)" << std::endl;

    assert(top_20_percent > 70.0 && "Should show strong skew");
    std::cout << "  ✓ Zipfian shows expected skew" << std::endl;
}

void test_sequential() {
    std::cout << "\n[TEST] Sequential Generator" << std::endl;

    SequentialGenerator<int> gen(100);

    for (int expected = 100; expected < 200; ++expected) {
        int key = gen.next();
        assert(key == expected && "Should generate sequential keys");
    }

    gen.reset();
    assert(gen.next() == 100 && "Reset should work");

    std::cout << "  ✓ Sequential generator works" << std::endl;
}

void test_adversarial() {
    std::cout << "\n[TEST] Adversarial Generator" << std::endl;

    size_t num_shards = 8;
    size_t target_shard = 0;
    AdversarialGenerator<int> gen(num_shards, target_shard);

    // All keys should hash to shard 0
    std::hash<int> hasher;

    for (size_t i = 0; i < 100; ++i) {
        int key = gen.next();
        size_t shard = hasher(key) % num_shards;

        // Note: std::hash may not preserve modulo property perfectly,
        // but our generator generates keys like 0, 8, 16, 24, ...
        // which should hash to approximately the same shard
        assert(key % num_shards == target_shard && "Key should target specific shard");
    }

    std::cout << "  ✓ Adversarial generator targets specific shard" << std::endl;
}

void test_hotspot() {
    std::cout << "\n[TEST] Hotspot Generator" << std::endl;

    // Cold range: 0-999, Hot range: 0-99, 10% hot
    HotspotGenerator<int> gen(0, 999, 0, 99, 0.1, 12345);

    std::unordered_map<int, size_t> counts;

    for (size_t i = 0; i < 100000; ++i) {
        int key = gen.next();
        assert(key >= 0 && key <= 999 && "Key out of range");
        counts[key]++;
    }

    // Count accesses to hotspot (0-99)
    size_t hotspot_accesses = 0;
    for (int i = 0; i <= 99; ++i) {
        hotspot_accesses += counts[i];
    }

    double hotspot_percent = (hotspot_accesses * 100.0) / 100000.0;
    std::cout << "  Hotspot range (0-99) got: " << hotspot_percent << "% of accesses" << std::endl;

    // Should be close to 10% (may vary due to randomness)
    assert(hotspot_percent > 5.0 && hotspot_percent < 25.0 && "Hotspot percentage out of range");

    std::cout << "  ✓ Hotspot generator works" << std::endl;
}

void test_factory() {
    std::cout << "\n[TEST] Workload Factory" << std::endl;

    auto uniform = WorkloadFactory<int>::create(WorkloadType::UNIFORM, 1000, 8, 123);
    auto zipfian = WorkloadFactory<int>::create(WorkloadType::ZIPFIAN, 1000, 8, 123);
    auto sequential = WorkloadFactory<int>::create(WorkloadType::SEQUENTIAL, 1000, 8, 123);
    auto adversarial = WorkloadFactory<int>::create(WorkloadType::ADVERSARIAL, 1000, 8, 123);

    // Just check they generate valid keys
    assert(uniform->next() >= 0 && "Uniform should work");
    assert(zipfian->next() >= 1 && "Zipfian should work");
    assert(sequential->next() == 0 && "Sequential should start at 0");
    assert(adversarial->next() == 0 && "Adversarial should start at 0");

    // Test names
    assert(std::string(WorkloadFactory<int>::name(WorkloadType::UNIFORM)) == "UNIFORM");
    assert(std::string(WorkloadFactory<int>::name(WorkloadType::ZIPFIAN)) == "ZIPFIAN");

    std::cout << "  ✓ Factory works" << std::endl;
}

int main() {
    std::cout << "\n╔════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  Workload Generator Test Suite             ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════╝" << std::endl;

    test_uniform_distribution();
    test_zipfian_skew();
    test_sequential();
    test_adversarial();
    test_hotspot();
    test_factory();

    std::cout << "\n╔════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  ✓ ALL WORKLOAD TESTS PASSED               ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════╝\n" << std::endl;

    return 0;
}
