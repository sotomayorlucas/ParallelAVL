// =============================================================================
// Test: Distributed AVL - Multi-Node Cluster Simulation
// =============================================================================

#include <iostream>
#include <iomanip>
#include <cassert>
#include <random>
#include <chrono>

#include "../include/DistributedAVL.hpp"

using namespace distributed;
using namespace std::chrono;

void print_test(const std::string& name, bool passed) {
    std::cout << "  [" << (passed ? "PASS" : "FAIL") << "] " << name << "\n";
}

// -----------------------------------------------------------------------------
// Test 1: Cluster Creation
// -----------------------------------------------------------------------------

void test_cluster_creation() {
    std::cout << "\n=== Test 1: Cluster Creation ===\n";
    
    ClusterConfig config;
    config.num_nodes = 4;
    config.shards_per_node = 8;
    config.replication_factor = 1;
    config.consistency = ConsistencyLevel::STRONG;
    
    ClusterManager<int, int> cluster(config);
    
    print_test("Cluster created with 4 nodes", cluster.get_num_nodes() == 4);
    
    auto* coordinator = cluster.get_coordinator();
    print_test("Coordinator initialized", coordinator != nullptr);
    print_test("Total shards = 32", coordinator->get_total_shards() == 32);
    
    // Verificar que cada nodo existe
    bool all_nodes_exist = true;
    for (size_t i = 0; i < 4; ++i) {
        if (cluster.get_node(i) == nullptr) {
            all_nodes_exist = false;
            break;
        }
    }
    print_test("All nodes accessible", all_nodes_exist);
}

// -----------------------------------------------------------------------------
// Test 2: Key Routing
// -----------------------------------------------------------------------------

void test_key_routing() {
    std::cout << "\n=== Test 2: Key Routing ===\n";
    
    ClusterConfig config;
    config.num_nodes = 4;
    config.shards_per_node = 8;
    
    ClusterManager<int, int> cluster(config);
    auto* coordinator = cluster.get_coordinator();
    
    // Verificar que las keys se distribuyen entre nodos
    std::vector<int> node_counts(4, 0);
    for (int key = 0; key < 10000; ++key) {
        NodeId node = coordinator->get_primary_node(key);
        node_counts[node]++;
    }
    
    // Verificar distribución razonablemente uniforme
    double avg = 10000.0 / 4;
    bool well_distributed = true;
    for (int count : node_counts) {
        if (count < avg * 0.5 || count > avg * 1.5) {
            well_distributed = false;
        }
    }
    
    print_test("Keys distributed across nodes", well_distributed);
    
    std::cout << "    Distribution: ";
    for (size_t i = 0; i < node_counts.size(); ++i) {
        std::cout << "N" << i << "=" << node_counts[i] << " ";
    }
    std::cout << "\n";
    
    // Verificar que la misma key siempre va al mismo nodo
    bool consistent = true;
    for (int key = 0; key < 100; ++key) {
        NodeId node1 = coordinator->get_primary_node(key);
        NodeId node2 = coordinator->get_primary_node(key);
        if (node1 != node2) {
            consistent = false;
            break;
        }
    }
    print_test("Routing is deterministic", consistent);
}

// -----------------------------------------------------------------------------
// Test 3: Local Operations
// -----------------------------------------------------------------------------

void test_local_operations() {
    std::cout << "\n=== Test 3: Local Operations ===\n";
    
    ClusterConfig config;
    config.num_nodes = 4;
    config.shards_per_node = 4;
    
    ClusterManager<int, int> cluster(config);
    
    // Insert some keys
    for (int i = 0; i < 1000; ++i) {
        cluster.insert(i, i * 10);
    }
    
    auto stats = cluster.get_stats();
    print_test("Total elements = 1000", stats.total_elements == 1000);
    
    // Verify distribution
    bool all_nodes_have_data = true;
    for (size_t count : stats.elements_per_node) {
        if (count == 0) {
            all_nodes_have_data = false;
        }
    }
    print_test("All nodes have data", all_nodes_have_data);
    
    std::cout << "    Elements per node: ";
    for (size_t i = 0; i < stats.elements_per_node.size(); ++i) {
        std::cout << "N" << i << "=" << stats.elements_per_node[i] << " ";
    }
    std::cout << "\n";
    std::cout << "    Balance score: " << std::fixed << std::setprecision(1) 
              << (stats.balance_score * 100) << "%\n";
    
    // Verify get
    bool reads_work = true;
    for (int i = 0; i < 100; ++i) {
        auto val = cluster.get(i);
        if (!val.has_value() || *val != i * 10) {
            reads_work = false;
            break;
        }
    }
    print_test("Reads return correct values", reads_work);
    
    // Verify remove
    for (int i = 0; i < 100; ++i) {
        cluster.remove(i);
    }
    stats = cluster.get_stats();
    print_test("Removes work (900 remaining)", stats.total_elements == 900);
}

// -----------------------------------------------------------------------------
// Test 4: Node Health Tracking
// -----------------------------------------------------------------------------

void test_node_health() {
    std::cout << "\n=== Test 4: Node Health Tracking ===\n";
    
    ClusterConfig config;
    config.num_nodes = 4;
    config.shards_per_node = 4;
    
    ClusterManager<int, int> cluster(config);
    auto* coordinator = cluster.get_coordinator();
    
    // Verificar estado inicial
    auto health = coordinator->get_node_health(0);
    print_test("Node 0 initially healthy", health.status == NodeStatus::HEALTHY);
    
    // Simular degradación
    coordinator->update_node_health(1, NodeStatus::DEGRADED, 0.8);
    health = coordinator->get_node_health(1);
    print_test("Node 1 marked degraded", health.status == NodeStatus::DEGRADED);
    print_test("Load factor updated", health.load_factor == 0.8);
    
    // Simular fallo
    coordinator->update_node_health(2, NodeStatus::OFFLINE);
    health = coordinator->get_node_health(2);
    print_test("Node 2 marked offline", health.status == NodeStatus::OFFLINE);
    
    // Obtener estado completo del cluster
    auto cluster_state = coordinator->get_cluster_state();
    print_test("Cluster state has 4 nodes", cluster_state.size() == 4);
}

// -----------------------------------------------------------------------------
// Test 5: Coordinator Versioning
// -----------------------------------------------------------------------------

void test_versioning() {
    std::cout << "\n=== Test 5: Version Vector ===\n";
    
    ClusterConfig config;
    config.num_nodes = 4;
    config.shards_per_node = 4;
    
    ClusterManager<int, int> cluster(config);
    auto* coordinator = cluster.get_coordinator();
    
    Version v0 = coordinator->get_local_version();
    print_test("Initial version is 0", v0 == 0);
    
    Version v1 = coordinator->increment_version();
    print_test("Increment returns 1", v1 == 1);
    
    Version v2 = coordinator->increment_version();
    print_test("Second increment returns 2", v2 == 2);
    
    Version current = coordinator->get_local_version();
    print_test("Current version is 2", current == 2);
}

// -----------------------------------------------------------------------------
// Test 6: Consistency Levels Configuration
// -----------------------------------------------------------------------------

void test_consistency_config() {
    std::cout << "\n=== Test 6: Consistency Configuration ===\n";
    
    // Strong consistency
    {
        ClusterConfig config;
        config.consistency = ConsistencyLevel::STRONG;
        config.replication_factor = 3;
        ClusterManager<int, int> cluster(config);
        
        print_test("Strong consistency configured", 
            cluster.get_coordinator()->get_config().consistency == ConsistencyLevel::STRONG);
        print_test("Replication factor = 3",
            cluster.get_coordinator()->get_config().replication_factor == 3);
    }
    
    // Eventual consistency
    {
        ClusterConfig config;
        config.consistency = ConsistencyLevel::EVENTUAL;
        config.replication = ReplicationMode::ASYNC;
        ClusterManager<int, int> cluster(config);
        
        print_test("Eventual consistency configured",
            cluster.get_coordinator()->get_config().consistency == ConsistencyLevel::EVENTUAL);
        print_test("Async replication configured",
            cluster.get_coordinator()->get_config().replication == ReplicationMode::ASYNC);
    }
}

// -----------------------------------------------------------------------------
// Test 7: Performance - Cluster vs Single Node
// -----------------------------------------------------------------------------

void test_performance() {
    std::cout << "\n=== Test 7: Performance Comparison ===\n";
    
    const size_t NUM_OPS = 100000;
    std::mt19937 rng(42);
    std::uniform_int_distribution<> key_dist(0, 999999);
    
    // Single node (baseline)
    double single_node_ms;
    {
        AVLTreeParallelV2<int, int> tree(8);
        
        auto start = high_resolution_clock::now();
        for (size_t i = 0; i < NUM_OPS; ++i) {
            int key = key_dist(rng);
            tree.insert(key, key);
        }
        auto end = high_resolution_clock::now();
        single_node_ms = duration_cast<microseconds>(end - start).count() / 1000.0;
    }
    
    // 4-node cluster
    double cluster_ms;
    {
        ClusterConfig config;
        config.num_nodes = 4;
        config.shards_per_node = 8;
        ClusterManager<int, int> cluster(config);
        
        rng.seed(42);  // Reset RNG
        
        auto start = high_resolution_clock::now();
        for (size_t i = 0; i < NUM_OPS; ++i) {
            int key = key_dist(rng);
            cluster.insert(key, key);
        }
        auto end = high_resolution_clock::now();
        cluster_ms = duration_cast<microseconds>(end - start).count() / 1000.0;
    }
    
    std::cout << "    Single node: " << std::fixed << std::setprecision(2) 
              << single_node_ms << " ms\n";
    std::cout << "    4-node cluster: " << cluster_ms << " ms\n";
    std::cout << "    Overhead: " << std::setprecision(1) 
              << ((cluster_ms / single_node_ms - 1) * 100) << "%\n";
    
    // El overhead debería ser razonable (<50% para routing local)
    print_test("Cluster overhead < 100%", cluster_ms < single_node_ms * 2);
}

// -----------------------------------------------------------------------------
// Test 8: Replication Setup
// -----------------------------------------------------------------------------

void test_replication_setup() {
    std::cout << "\n=== Test 8: Replication Setup ===\n";
    
    ClusterConfig config;
    config.num_nodes = 4;
    config.shards_per_node = 4;
    config.replication_factor = 2;  // 1 primary + 1 replica
    
    ClusterManager<int, int> cluster(config);
    auto* coordinator = cluster.get_coordinator();
    
    // Verificar que las réplicas están configuradas
    int key = 42;
    NodeId primary = coordinator->get_primary_node(key);
    auto replicas = coordinator->get_replica_nodes(key);
    
    print_test("Primary node assigned", primary < 4);
    print_test("Replica assigned", !replicas.empty());
    
    if (!replicas.empty()) {
        print_test("Replica different from primary", replicas[0] != primary);
        std::cout << "    Key " << key << ": Primary=N" << primary 
                  << ", Replica=N" << replicas[0] << "\n";
    }
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

int main() {
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║          DISTRIBUTED AVL - TEST SUITE                        ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
    
    test_cluster_creation();
    test_key_routing();
    test_local_operations();
    test_node_health();
    test_versioning();
    test_consistency_config();
    test_performance();
    test_replication_setup();
    
    std::cout << "\n╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                    ALL TESTS COMPLETE                        ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n\n";
    
    return 0;
}
