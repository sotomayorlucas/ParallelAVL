#ifndef AVL_TREE_PARALLEL_H
#define AVL_TREE_PARALLEL_H

#include "BaseTree.h"
#include "AVLTree.h"
#include <vector>
#include <mutex>
#include <memory>
#include <functional>
#include <thread>
#include <stdexcept>
#include <iostream>
#include <iomanip>
#include <cmath>
#include <algorithm>

// Parallel AVL Tree: Tree-of-Trees Architecture
//
// Estrategia: N árboles independientes (uno por core)
// - Cada árbol tiene su propio global lock (simple, eficiente)
// - Super-estructura para routing a árbol correcto
// - Operaciones en diferentes árboles = paralelismo REAL
// - Sin contención entre árboles
//
// Arquitectura:
//          [Routing Layer]
//         /    |    \    \
//    Tree0  Tree1  Tree2  ... TreeN
//    Lock0  Lock1  Lock2      LockN
//      ↓      ↓      ↓          ↓
//   Core 0  Core 1 Core 2   Core N
//
// N threads trabajando en N árboles diferentes = N operaciones paralelas!

template<typename Key, typename Value = Key>
class AVLTreeParallel : public BaseTree<Key, Value> {
public:
    enum class RoutingStrategy {
        HASH,       // key % num_trees (distribución uniforme)
        RANGE       // Dividir rango de keys en particiones
    };

private:
    // Cada "shard" es un árbol AVL independiente con su lock
    struct TreeShard {
        AVLTree<Key, Value> tree;
        mutable std::mutex lock;
        size_t local_size;

        // Para range-based routing
        Key min_key;
        Key max_key;

        TreeShard() : local_size(0), min_key(Key()), max_key(Key()) {}
    };

    std::vector<std::unique_ptr<TreeShard>> shards_;
    size_t num_shards_;
    RoutingStrategy routing_;

    // Para range-based: dividir espacio de keys
    Key global_min_;
    Key global_max_;

    // Para thread affinity (opcional)
    bool use_thread_affinity_;

    // Routing: Determinar a qué árbol va esta key
    size_t getShardIndex(const Key& key) const {
        if (routing_ == RoutingStrategy::HASH) {
            // Hash-based: distribución uniforme
            return std::hash<Key>{}(key) % num_shards_;
        } else {
            // Range-based: keys consecutivas van al mismo árbol
            // Esto es mejor para range queries
            // Simplificado: usar modulo del valor
            // En producción: usar rangos definidos
            return static_cast<size_t>(key) % num_shards_;
        }
    }

public:
    // Constructor: num_shards = número de cores disponibles
    AVLTreeParallel(
        size_t num_shards = std::thread::hardware_concurrency(),
        RoutingStrategy routing = RoutingStrategy::HASH,
        bool thread_affinity = false)
        : num_shards_(num_shards),
          routing_(routing),
          use_thread_affinity_(thread_affinity) {

        // Crear N árboles independientes
        for (size_t i = 0; i < num_shards_; ++i) {
            shards_.push_back(std::make_unique<TreeShard>());
        }
    }

    ~AVLTreeParallel() override = default;

    // Insert: Route a árbol correcto, lock SOLO ese árbol
    void insert(const Key& key, const Value& value = Value()) override {
        size_t shard_idx = getShardIndex(key);
        auto& shard = shards_[shard_idx];

        std::lock_guard<std::mutex> lock(shard->lock);

        size_t old_size = shard->tree.size();
        shard->tree.insert(key, value);
        size_t new_size = shard->tree.size();

        if (new_size > old_size) {
            shard->local_size++;
        }
    }

    // Remove: Solo lock el árbol que contiene la key
    void remove(const Key& key) override {
        size_t shard_idx = getShardIndex(key);
        auto& shard = shards_[shard_idx];

        std::lock_guard<std::mutex> lock(shard->lock);

        size_t old_size = shard->tree.size();
        shard->tree.remove(key);
        size_t new_size = shard->tree.size();

        if (new_size < old_size) {
            shard->local_size--;
        }
    }

    // Contains: Solo lock el árbol relevante
    bool contains(const Key& key) const override {
        size_t shard_idx = getShardIndex(key);
        auto& shard = shards_[shard_idx];

        std::lock_guard<std::mutex> lock(shard->lock);
        return shard->tree.contains(key);
    }

    // Get: Solo lock el árbol relevante
    const Value& get(const Key& key) const override {
        size_t shard_idx = getShardIndex(key);
        auto& shard = shards_[shard_idx];

        std::lock_guard<std::mutex> lock(shard->lock);
        return shard->tree.get(key);
    }

    // Size: Sumar tamaños de todos los árboles
    std::size_t size() const override {
        std::size_t total = 0;

        // Lock todos los shards para consistencia
        std::vector<std::unique_lock<std::mutex>> locks;
        for (const auto& shard : shards_) {
            locks.emplace_back(shard->lock);
        }

        for (const auto& shard : shards_) {
            total += shard->local_size;
        }

        return total;
    }

    void clear() {
        for (auto& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard->lock);
            shard->tree.clear();
            shard->local_size = 0;
        }
    }

    // Estadísticas de balanceo entre árboles
    struct ShardStats {
        size_t shard_id;
        size_t element_count;
        double load_percentage;
    };

    std::vector<ShardStats> getShardStats() const {
        std::vector<ShardStats> stats;
        size_t total = size();

        for (size_t i = 0; i < num_shards_; ++i) {
            std::lock_guard<std::mutex> lock(shards_[i]->lock);

            ShardStats s;
            s.shard_id = i;
            s.element_count = shards_[i]->local_size;
            s.load_percentage = total > 0 ? (100.0 * s.element_count / total) : 0.0;

            stats.push_back(s);
        }

        return stats;
    }

    // Información sobre la arquitectura
    struct ArchitectureInfo {
        size_t num_shards;
        size_t hardware_concurrency;
        RoutingStrategy routing;
        bool thread_affinity;
        size_t total_elements;
        double avg_elements_per_shard;
        double load_balance_score;  // 1.0 = perfectamente balanceado
    };

    ArchitectureInfo getArchitectureInfo() const {
        ArchitectureInfo info;
        info.num_shards = num_shards_;
        info.hardware_concurrency = std::thread::hardware_concurrency();
        info.routing = routing_;
        info.thread_affinity = use_thread_affinity_;
        info.total_elements = size();
        info.avg_elements_per_shard = static_cast<double>(info.total_elements) / num_shards_;

        // Calcular score de balanceo (1.0 = perfecto)
        auto stats = getShardStats();
        double variance = 0.0;
        for (const auto& s : stats) {
            double diff = s.element_count - info.avg_elements_per_shard;
            variance += diff * diff;
        }
        variance /= num_shards_;

        // Score: 1.0 - (std_dev / avg)
        // Si std_dev = 0, score = 1.0 (perfecto)
        // Si std_dev = avg, score = 0.0 (terrible)
        double std_dev = std::sqrt(variance);
        info.load_balance_score = info.avg_elements_per_shard > 0
            ? std::max(0.0, 1.0 - (std_dev / info.avg_elements_per_shard))
            : 1.0;

        return info;
    }

    // Helper: Extraer todos los elementos de un shard
    std::vector<std::pair<Key, Value>> extractAllElements(TreeShard* shard) {
        std::vector<std::pair<Key, Value>> elements;
        extractElementsRec(shard->tree.getRoot(), elements);
        return elements;
    }

private:
    // Helper recursivo para extraer elementos
    void extractElementsRec(typename AVLTree<Key, Value>::Node* node,
                           std::vector<std::pair<Key, Value>>& elements) {
        if (!node) return;
        extractElementsRec(node->left, elements);
        elements.push_back({node->key, node->value});
        extractElementsRec(node->right, elements);
    }

public:
    // Rebalanceo entre árboles (si un árbol tiene mucho más que otros)
    void rebalanceShards(double threshold = 2.0) {
        // Lock todos los shards para operación atómica
        std::vector<std::unique_lock<std::mutex>> locks;
        for (auto& shard : shards_) {
            locks.emplace_back(shard->lock);
        }

        auto info = getArchitectureInfo();

        // Si el balanceo es bueno, no hacer nada
        if (info.load_balance_score >= 0.8) {
            return;
        }

        // Identificar shards sobrecargados y subcargados
        struct ShardLoad {
            size_t index;
            size_t size;
        };

        std::vector<ShardLoad> loads;
        for (size_t i = 0; i < num_shards_; ++i) {
            loads.push_back({i, shards_[i]->local_size});
        }

        // Ordenar por tamaño
        std::sort(loads.begin(), loads.end(),
                 [](const ShardLoad& a, const ShardLoad& b) {
                     return a.size > b.size;
                 });

        // Estrategia: Migrar elementos del más lleno al más vacío
        size_t overloaded_idx = loads[0].index;
        size_t underloaded_idx = loads[num_shards_ - 1].index;

        // Si el más lleno tiene > threshold * promedio, redistribuir
        if (loads[0].size > threshold * info.avg_elements_per_shard &&
            loads[0].size > 0) {

            auto& overloaded = shards_[overloaded_idx];
            auto& underloaded = shards_[underloaded_idx];

            // Calcular cuántos elementos mover
            size_t excess = overloaded->local_size - static_cast<size_t>(info.avg_elements_per_shard);
            size_t to_move = std::min(excess / 2, overloaded->local_size / 2);

            // Solo mover los elementos necesarios (no todos)
            std::vector<std::pair<Key, Value>> elements = extractAllElements(overloaded.get());

            // Limpiar el shard sobrecargado
            overloaded->tree.clear();
            overloaded->local_size = 0;

            // Quedarse con (size - to_move) elementos, mover to_move al otro shard
            size_t keep_count = elements.size() - to_move;

            for (size_t i = 0; i < elements.size(); ++i) {
                if (i < keep_count) {
                    overloaded->tree.insert(elements[i].first, elements[i].second);
                    overloaded->local_size++;
                } else {
                    underloaded->tree.insert(elements[i].first, elements[i].second);
                    underloaded->local_size++;
                }
            }
        }
    }

    // Rebalanceo automático si es necesario
    bool shouldRebalance(double threshold = 0.7) const {
        auto info = getArchitectureInfo();
        return info.load_balance_score < threshold;
    }

    // Para debugging: imprimir distribución
    void printDistribution() const {
        auto stats = getShardStats();
        auto info = getArchitectureInfo();

        std::cout << "\n╔════════════════════════════════════════╗" << std::endl;
        std::cout << "║  Parallel Tree Architecture            ║" << std::endl;
        std::cout << "╚════════════════════════════════════════╝\n" << std::endl;

        std::cout << "Shards: " << info.num_shards << std::endl;
        std::cout << "Hardware threads: " << info.hardware_concurrency << std::endl;
        std::cout << "Total elements: " << info.total_elements << std::endl;
        std::cout << "Avg per shard: " << info.avg_elements_per_shard << std::endl;
        std::cout << "Balance score: " << std::fixed << std::setprecision(2)
                  << (info.load_balance_score * 100) << "%" << std::endl;

        std::cout << "\nShard Distribution:" << std::endl;
        for (const auto& s : stats) {
            std::cout << "  Shard " << s.shard_id << ": "
                      << std::setw(6) << s.element_count << " elements ("
                      << std::fixed << std::setprecision(1) << s.load_percentage << "%)"
                      << std::endl;
        }
    }

    size_t getNumShards() const { return num_shards_; }
};

#endif // AVL_TREE_PARALLEL_H
