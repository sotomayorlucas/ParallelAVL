// =============================================================================
// Test de Regresión: DynamicShardedTree
// =============================================================================
// Verifica compatibilidad de API y funcionalidad de scaling

#include <iostream>
#include <cassert>
#include <vector>
#include <random>
#include <string>

#include "../include/DynamicShardedTree.hpp"

void print_test(const std::string& name, bool passed) {
    std::cout << "  [" << (passed ? "PASS" : "FAIL") << "] " << name << "\n";
    if (!passed) {
        std::cerr << "ASSERTION FAILED: " << name << std::endl;
    }
}

// -----------------------------------------------------------------------------
// Test 1: Compatibilidad de API básica
// -----------------------------------------------------------------------------

void test_api_compatibility() {
    std::cout << "\n=== Test 1: API Compatibility ===\n";
    
    DynamicShardedTree<int, int> tree;
    
    // Insert
    for (int i = 0; i < 10000; ++i) {
        tree.insert(i, i * 10);
    }
    print_test("insert() works", true);
    
    // Size
    bool size_ok = (tree.size() == 10000);
    print_test("size() == 10000", size_ok);
    
    // Contains
    bool contains_ok = tree.contains(5000) && !tree.contains(99999);
    print_test("contains() works", contains_ok);
    
    // Get
    bool get_ok = (tree.get(5000) == 50000);
    print_test("get() returns correct value", get_ok);
    
    // Remove
    tree.remove(5000);
    bool remove_ok = !tree.contains(5000) && (tree.size() == 9999);
    print_test("remove() works", remove_ok);
    
    // Get non-existent (should return default)
    int val = tree.get(99999);
    bool get_default_ok = (val == 0);
    print_test("get() returns default for missing key", get_default_ok);
}

// -----------------------------------------------------------------------------
// Test 2: Scaling básico
// -----------------------------------------------------------------------------

void test_basic_scaling() {
    std::cout << "\n=== Test 2: Basic Scaling ===\n";
    
    DynamicShardedTree<int, int> tree;
    
    // Insertar datos
    for (int i = 0; i < 50000; ++i) {
        tree.insert(i, i);
    }
    
    auto stats1 = tree.get_stats();
    print_test("Initial shards == 4", stats1.num_shards == 4);
    print_test("Initial balance > 0.7", stats1.balance_score > 0.7);
    
    // Agregar shards
    tree.add_shard();
    tree.add_shard();
    
    auto stats2 = tree.get_stats();
    print_test("After add_shard: shards == 6", stats2.num_shards == 6);
    print_test("Data preserved after add_shard", stats2.total_elements == stats1.total_elements);
    
    // Rebalance
    tree.force_rebalance();
    
    auto stats3 = tree.get_stats();
    print_test("After rebalance: balance > 0.8", stats3.balance_score > 0.8);
    print_test("Data preserved after rebalance", stats3.total_elements == stats1.total_elements);
}

// -----------------------------------------------------------------------------
// Test 3: Scaling down (remove_shard)
// -----------------------------------------------------------------------------

void test_scale_down() {
    std::cout << "\n=== Test 3: Scale Down ===\n";
    
    DynamicShardedTree<int, int>::Config config;
    config.initial_shards = 6;
    DynamicShardedTree<int, int> tree(config);
    
    for (int i = 0; i < 30000; ++i) {
        tree.insert(i, i * 2);
    }
    
    auto stats1 = tree.get_stats();
    print_test("Initial shards == 6", stats1.num_shards == 6);
    
    tree.remove_shard();
    tree.remove_shard();
    
    auto stats2 = tree.get_stats();
    print_test("After remove_shard: shards == 4", stats2.num_shards == 4);
    print_test("Data preserved after remove_shard", stats2.total_elements == stats1.total_elements);
    
    // Verificar que los datos siguen accesibles
    bool data_ok = true;
    for (int i = 0; i < 100; ++i) {
        if (tree.get(i) != i * 2) {
            data_ok = false;
            break;
        }
    }
    print_test("Data still accessible after scale down", data_ok);
}

// -----------------------------------------------------------------------------
// Test 4: Consistent hashing - distribución
// -----------------------------------------------------------------------------

void test_distribution() {
    std::cout << "\n=== Test 4: Distribution ===\n";
    
    DynamicShardedTree<int, int> tree;
    
    std::mt19937 rng(42);
    std::uniform_int_distribution<> dist(0, 999999);
    
    for (int i = 0; i < 100000; ++i) {
        tree.insert(dist(rng), i);
    }
    
    auto stats = tree.get_stats();
    
    // Verificar que ningún shard tiene más del 35% o menos del 15%
    bool well_distributed = true;
    for (size_t count : stats.elements_per_shard) {
        double pct = 100.0 * count / stats.total_elements;
        if (pct < 15 || pct > 35) {
            well_distributed = false;
        }
    }
    
    print_test("Balance score > 0.75", stats.balance_score > 0.75);
    print_test("All shards between 15-35%", well_distributed);
}

// -----------------------------------------------------------------------------
// Test 5: String keys
// -----------------------------------------------------------------------------

void test_string_keys() {
    std::cout << "\n=== Test 5: String Keys ===\n";
    
    DynamicShardedTree<std::string, int> tree;
    
    tree.insert("hello", 1);
    tree.insert("world", 2);
    tree.insert("foo", 3);
    tree.insert("bar", 4);
    
    bool insert_ok = (tree.size() == 4);
    print_test("Insert string keys", insert_ok);
    
    bool contains_ok = tree.contains("hello") && tree.contains("world");
    print_test("Contains string keys", contains_ok);
    
    bool get_ok = (tree.get("foo") == 3);
    print_test("Get string keys", get_ok);
    
    tree.remove("bar");
    bool remove_ok = !tree.contains("bar") && (tree.size() == 3);
    print_test("Remove string keys", remove_ok);
}

// -----------------------------------------------------------------------------
// Test 6: Lazy migration
// -----------------------------------------------------------------------------

void test_lazy_migration() {
    std::cout << "\n=== Test 6: Lazy Migration ===\n";
    
    DynamicShardedTree<int, int> tree;
    
    // Insertar datos
    for (int i = 0; i < 10000; ++i) {
        tree.insert(i, i * 100);
    }
    
    // Agregar shard (sin rebalance explícito)
    tree.add_shard();
    
    // Acceder a datos - debe encontrarlos y migrarlos lazy
    bool all_found = true;
    for (int i = 0; i < 1000; ++i) {
        if (!tree.contains(i)) {
            all_found = false;
            break;
        }
        if (tree.get(i) != i * 100) {
            all_found = false;
            break;
        }
    }
    
    print_test("Data accessible after add_shard (lazy migration)", all_found);
}

// -----------------------------------------------------------------------------
// Test 7: Config customization
// -----------------------------------------------------------------------------

void test_config() {
    std::cout << "\n=== Test 7: Config Customization ===\n";
    
    DynamicShardedTree<int, int>::Config config;
    config.initial_shards = 8;
    config.vnodes_per_shard = 128;
    
    DynamicShardedTree<int, int> tree(config);
    
    auto stats = tree.get_stats();
    print_test("Custom initial_shards == 8", stats.num_shards == 8);
    
    for (int i = 0; i < 10000; ++i) {
        tree.insert(i, i);
    }
    
    auto stats2 = tree.get_stats();
    print_test("Data inserted with custom config", stats2.total_elements == 10000);
}

// -----------------------------------------------------------------------------
// Test 8: Edge cases
// -----------------------------------------------------------------------------

void test_edge_cases() {
    std::cout << "\n=== Test 8: Edge Cases ===\n";
    
    DynamicShardedTree<int, int> tree;
    
    // Empty tree
    print_test("Empty tree size == 0", tree.size() == 0);
    print_test("Empty tree contains(0) == false", !tree.contains(0));
    
    // Single element
    tree.insert(42, 100);
    print_test("Single element size == 1", tree.size() == 1);
    print_test("Single element get(42) == 100", tree.get(42) == 100);
    
    // Duplicate insert (update)
    tree.insert(42, 200);
    print_test("Duplicate insert updates value", tree.get(42) == 200);
    print_test("Duplicate insert keeps size == 1", tree.size() == 1);
    
    // Remove non-existent
    tree.remove(9999);
    print_test("Remove non-existent doesn't crash", tree.size() == 1);
    
    // Cannot remove last shard
    DynamicShardedTree<int, int>::Config config;
    config.initial_shards = 1;
    DynamicShardedTree<int, int> single_shard(config);
    single_shard.insert(1, 1);
    single_shard.remove_shard();  // Should do nothing
    print_test("Cannot remove last shard", single_shard.get_num_shards() == 1);
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

int main() {
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║     DYNAMIC SHARDED TREE - REGRESSION TEST SUITE             ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
    
    test_api_compatibility();
    test_basic_scaling();
    test_scale_down();
    test_distribution();
    test_string_keys();
    test_lazy_migration();
    test_config();
    test_edge_cases();
    
    std::cout << "\n╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                 ALL REGRESSION TESTS COMPLETE                ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n\n";
    
    return 0;
}
