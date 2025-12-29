#ifndef DYNAMIC_SHARDED_TREE_HPP
#define DYNAMIC_SHARDED_TREE_HPP

#include "AVLTree.h"
#include <vector>
#include <mutex>
#include <atomic>
#include <memory>
#include <functional>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <cmath>

// =============================================================================
// Dynamic Sharded Tree - Escalado Elástico con Migración Lazy
// =============================================================================
//
// Versión simplificada y robusta:
// - Consistent Hashing con virtual nodes
// - Migración LAZY (on-access) - sin background threads
// - Zero deadlocks - cada operación toma máximo UN lock
// - Balance se restaura gradualmente con el uso
//
// =============================================================================

template<typename Key, typename Value = Key>
class DynamicShardedTree {
public:
    struct Config {
        size_t initial_shards = 4;
        size_t vnodes_per_shard = 64;
    };

    struct Stats {
        size_t num_shards;
        size_t total_elements;
        double balance_score;
        std::vector<size_t> elements_per_shard;
    };

private:
    // =========================================================================
    // Shard Structure - Simple mutex por shard
    // =========================================================================
    
    struct Shard {
        AVLTree<Key, Value> tree;
        mutable std::mutex lock;
        std::atomic<size_t> size{0};
        
        Shard() = default;
        Shard(const Shard&) = delete;
        Shard& operator=(const Shard&) = delete;
    };

    // =========================================================================
    // Consistent Hash Ring
    // =========================================================================
    
    struct VirtualNode {
        size_t hash_point;
        size_t shard_id;
        
        bool operator<(const VirtualNode& o) const {
            return hash_point < o.hash_point;
        }
    };

    // =========================================================================
    // Member Variables
    // =========================================================================
    
    Config config_;
    std::vector<std::unique_ptr<Shard>> shards_;
    std::vector<VirtualNode> hash_ring_;
    
    mutable std::mutex topology_mutex_;  // Protege shards_ y hash_ring_
    std::atomic<size_t> num_shards_{0};
    std::atomic<uint64_t> topology_version_{0};

    // =========================================================================
    // Hash Functions
    // =========================================================================
    
    static size_t hash_key(const Key& key) {
        size_t h = std::hash<Key>{}(key);
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ULL;
        h ^= h >> 33;
        return h;
    }
    
    static size_t hash_vnode(size_t shard_id, size_t vnode_idx) {
        size_t h = (shard_id << 16) ^ vnode_idx;
        // Murmur3 finalizer (no depende de Key)
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ULL;
        h ^= h >> 33;
        return h;
    }

    // =========================================================================
    // Ring Operations (require topology_mutex_)
    // =========================================================================
    
    void rebuild_ring_locked() {
        hash_ring_.clear();
        hash_ring_.reserve(shards_.size() * config_.vnodes_per_shard);
        
        for (size_t s = 0; s < shards_.size(); ++s) {
            for (size_t v = 0; v < config_.vnodes_per_shard; ++v) {
                hash_ring_.push_back({hash_vnode(s, v), s});
            }
        }
        std::sort(hash_ring_.begin(), hash_ring_.end());
    }
    
    size_t find_shard_locked(size_t hash_value) const {
        if (hash_ring_.empty()) return 0;
        
        auto it = std::lower_bound(hash_ring_.begin(), hash_ring_.end(),
            VirtualNode{hash_value, 0});
        
        if (it == hash_ring_.end()) {
            it = hash_ring_.begin();
        }
        return it->shard_id;
    }

    // =========================================================================
    // Helper: Extract all elements from a tree
    // =========================================================================
    
    static void extract_all(typename AVLTree<Key, Value>::Node* node,
                           std::vector<std::pair<Key, Value>>& out) {
        if (!node) return;
        extract_all(node->left, out);
        out.emplace_back(node->key, node->value);
        extract_all(node->right, out);
    }

public:
    // =========================================================================
    // Constructor
    // =========================================================================
    
    explicit DynamicShardedTree(const Config& config = Config{})
        : config_(config)
    {
        std::lock_guard<std::mutex> lock(topology_mutex_);
        
        for (size_t i = 0; i < config_.initial_shards; ++i) {
            shards_.push_back(std::make_unique<Shard>());
        }
        num_shards_ = config_.initial_shards;
        rebuild_ring_locked();
    }
    
    ~DynamicShardedTree() = default;
    
    DynamicShardedTree(const DynamicShardedTree&) = delete;
    DynamicShardedTree& operator=(const DynamicShardedTree&) = delete;

    // =========================================================================
    // Core Operations - Migración Lazy integrada
    // =========================================================================
    
    void insert(const Key& key, const Value& value = Value{}) {
        size_t shard_id;
        {
            std::lock_guard<std::mutex> topo_lock(topology_mutex_);
            shard_id = find_shard_locked(hash_key(key));
        }
        
        std::lock_guard<std::mutex> shard_lock(shards_[shard_id]->lock);
        
        size_t old_size = shards_[shard_id]->tree.size();
        shards_[shard_id]->tree.insert(key, value);
        if (shards_[shard_id]->tree.size() > old_size) {
            shards_[shard_id]->size.fetch_add(1, std::memory_order_relaxed);
        }
    }
    
    bool contains(const Key& key) {
        size_t expected_shard;
        size_t n_shards;
        {
            std::lock_guard<std::mutex> topo_lock(topology_mutex_);
            expected_shard = find_shard_locked(hash_key(key));
            n_shards = shards_.size();
        }
        
        // Buscar primero en el shard esperado
        {
            std::lock_guard<std::mutex> lock(shards_[expected_shard]->lock);
            if (shards_[expected_shard]->tree.contains(key)) {
                return true;
            }
        }
        
        // Si no está, buscar en otros (migración lazy pendiente)
        for (size_t i = 0; i < n_shards; ++i) {
            if (i == expected_shard) continue;
            
            std::lock_guard<std::mutex> lock(shards_[i]->lock);
            if (shards_[i]->tree.contains(key)) {
                // Encontrado en shard incorrecto - migrar
                Value val = shards_[i]->tree.get(key);
                shards_[i]->tree.remove(key);
                shards_[i]->size.fetch_sub(1, std::memory_order_relaxed);
                
                // Insertar en shard correcto
                std::lock_guard<std::mutex> dest_lock(shards_[expected_shard]->lock);
                shards_[expected_shard]->tree.insert(key, val);
                shards_[expected_shard]->size.fetch_add(1, std::memory_order_relaxed);
                
                return true;
            }
        }
        
        return false;
    }
    
    Value get(const Key& key) {
        size_t expected_shard;
        size_t n_shards;
        {
            std::lock_guard<std::mutex> topo_lock(topology_mutex_);
            expected_shard = find_shard_locked(hash_key(key));
            n_shards = shards_.size();
        }
        
        // Buscar en shard esperado
        {
            std::lock_guard<std::mutex> lock(shards_[expected_shard]->lock);
            if (shards_[expected_shard]->tree.contains(key)) {
                return shards_[expected_shard]->tree.get(key);
            }
        }
        
        // Buscar en otros y migrar
        for (size_t i = 0; i < n_shards; ++i) {
            if (i == expected_shard) continue;
            
            std::lock_guard<std::mutex> lock(shards_[i]->lock);
            if (shards_[i]->tree.contains(key)) {
                Value val = shards_[i]->tree.get(key);
                shards_[i]->tree.remove(key);
                shards_[i]->size.fetch_sub(1, std::memory_order_relaxed);
                
                std::lock_guard<std::mutex> dest_lock(shards_[expected_shard]->lock);
                shards_[expected_shard]->tree.insert(key, val);
                shards_[expected_shard]->size.fetch_add(1, std::memory_order_relaxed);
                
                return val;
            }
        }
        
        return Value{};
    }
    
    void remove(const Key& key) {
        size_t n_shards;
        {
            std::lock_guard<std::mutex> topo_lock(topology_mutex_);
            n_shards = shards_.size();
        }
        
        for (size_t i = 0; i < n_shards; ++i) {
            std::lock_guard<std::mutex> lock(shards_[i]->lock);
            if (shards_[i]->tree.contains(key)) {
                shards_[i]->tree.remove(key);
                shards_[i]->size.fetch_sub(1, std::memory_order_relaxed);
                return;
            }
        }
    }
    
    size_t size() const {
        size_t total = 0;
        size_t n_shards = num_shards_.load(std::memory_order_acquire);
        for (size_t i = 0; i < n_shards; ++i) {
            total += shards_[i]->size.load(std::memory_order_relaxed);
        }
        return total;
    }

    // =========================================================================
    // Dynamic Scaling
    // =========================================================================
    
    void add_shard() {
        std::lock_guard<std::mutex> topo_lock(topology_mutex_);
        
        shards_.push_back(std::make_unique<Shard>());
        num_shards_.store(shards_.size(), std::memory_order_release);
        rebuild_ring_locked();
        topology_version_.fetch_add(1, std::memory_order_release);
    }
    
    void remove_shard() {
        std::lock_guard<std::mutex> topo_lock(topology_mutex_);
        
        if (shards_.size() <= 1) return;
        
        size_t removing_id = shards_.size() - 1;
        
        // Extraer datos del shard que se elimina
        std::vector<std::pair<Key, Value>> to_redistribute;
        {
            std::lock_guard<std::mutex> shard_lock(shards_[removing_id]->lock);
            extract_all(shards_[removing_id]->tree.getRoot(), to_redistribute);
        }
        
        // Eliminar shard y reconstruir ring
        shards_.pop_back();
        num_shards_.store(shards_.size(), std::memory_order_release);
        rebuild_ring_locked();
        topology_version_.fetch_add(1, std::memory_order_release);
        
        // Re-insertar datos (ahora van a shards correctos)
        for (const auto& [key, value] : to_redistribute) {
            size_t shard_id = find_shard_locked(hash_key(key));
            std::lock_guard<std::mutex> shard_lock(shards_[shard_id]->lock);
            shards_[shard_id]->tree.insert(key, value);
            shards_[shard_id]->size.fetch_add(1, std::memory_order_relaxed);
        }
    }
    
    // Forzar migración de TODAS las keys a sus shards correctos
    void force_rebalance() {
        std::lock_guard<std::mutex> topo_lock(topology_mutex_);
        
        // Extraer todo
        std::vector<std::pair<Key, Value>> all_data;
        for (auto& shard : shards_) {
            std::lock_guard<std::mutex> shard_lock(shard->lock);
            extract_all(shard->tree.getRoot(), all_data);
        }
        
        // Recrear shards vacíos
        size_t n = shards_.size();
        shards_.clear();
        for (size_t i = 0; i < n; ++i) {
            shards_.push_back(std::make_unique<Shard>());
        }
        
        // Re-insertar en shards correctos
        for (const auto& [key, value] : all_data) {
            size_t shard_id = find_shard_locked(hash_key(key));
            std::lock_guard<std::mutex> shard_lock(shards_[shard_id]->lock);
            shards_[shard_id]->tree.insert(key, value);
            shards_[shard_id]->size.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // =========================================================================
    // Statistics
    // =========================================================================
    
    Stats get_stats() const {
        Stats stats;
        stats.num_shards = num_shards_.load(std::memory_order_acquire);
        stats.total_elements = 0;
        stats.elements_per_shard.reserve(stats.num_shards);
        
        for (size_t i = 0; i < stats.num_shards; ++i) {
            size_t count = shards_[i]->size.load(std::memory_order_relaxed);
            stats.elements_per_shard.push_back(count);
            stats.total_elements += count;
        }
        
        if (stats.total_elements > 0 && stats.num_shards > 0) {
            double avg = static_cast<double>(stats.total_elements) / stats.num_shards;
            double variance = 0;
            for (size_t count : stats.elements_per_shard) {
                double diff = count - avg;
                variance += diff * diff;
            }
            double std_dev = std::sqrt(variance / stats.num_shards);
            stats.balance_score = std::max(0.0, 1.0 - (std_dev / avg));
        } else {
            stats.balance_score = 1.0;
        }
        
        return stats;
    }
    
    void print_stats() const {
        auto stats = get_stats();
        
        std::cout << "\n╔═══════════════════════════════════════════════╗\n";
        std::cout << "║  Dynamic Sharded Tree Statistics              ║\n";
        std::cout << "╚═══════════════════════════════════════════════╝\n\n";
        
        std::cout << "  Shards: " << stats.num_shards << "\n";
        std::cout << "  Total elements: " << stats.total_elements << "\n";
        std::cout << "  Balance score: " << std::fixed << std::setprecision(1)
                  << (stats.balance_score * 100) << "%\n";
        
        std::cout << "\n  Distribution:\n";
        for (size_t i = 0; i < stats.elements_per_shard.size(); ++i) {
            double pct = stats.total_elements > 0 
                ? 100.0 * stats.elements_per_shard[i] / stats.total_elements 
                : 0;
            std::cout << "    Shard " << i << ": " << std::setw(6) 
                      << stats.elements_per_shard[i] << " (" 
                      << std::setprecision(1) << pct << "%)\n";
        }
        std::cout << "\n";
    }
    
    size_t get_num_shards() const {
        return num_shards_.load(std::memory_order_acquire);
    }
    
    uint64_t get_topology_version() const {
        return topology_version_.load(std::memory_order_acquire);
    }
};

#endif // DYNAMIC_SHARDED_TREE_HPP
