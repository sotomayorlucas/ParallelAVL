#ifndef SHARD_HPP
#define SHARD_HPP

#include "AVLTree.h"
#include <mutex>
#include <atomic>
#include <optional>
#include <limits>

// TreeShard: Contenedor thread-safe para un AVL tree individual
// Mantiene estadísticas y metadatos para optimizaciones
template<typename Key, typename Value>
class TreeShard {
private:
    AVLTree<Key, Value> tree_;
    mutable std::mutex mutex_;

    // Estadísticas atómicas (lock-free reads)
    std::atomic<size_t> size_{0};
    std::atomic<size_t> insert_count_{0};
    std::atomic<size_t> remove_count_{0};
    mutable std::atomic<size_t> lookup_count_{0};  // mutable para actualizar en métodos const

    // Range query optimization: mantener min/max keys
    // Usa atomic para reads lock-free, pero updates requieren mutex
    std::atomic<Key> min_key_{std::numeric_limits<Key>::max()};
    std::atomic<Key> max_key_{std::numeric_limits<Key>::min()};
    std::atomic<bool> has_keys_{false};

    void update_bounds(const Key& key) {
        // Actualizar bounds (ya con lock del mutex_)
        if (!has_keys_.load(std::memory_order_relaxed)) {
            min_key_.store(key, std::memory_order_relaxed);
            max_key_.store(key, std::memory_order_relaxed);
            has_keys_.store(true, std::memory_order_release);
        } else {
            Key current_min = min_key_.load(std::memory_order_relaxed);
            Key current_max = max_key_.load(std::memory_order_relaxed);

            if (key < current_min) {
                min_key_.store(key, std::memory_order_relaxed);
            }
            if (key > current_max) {
                max_key_.store(key, std::memory_order_relaxed);
            }
        }
    }

    void recompute_bounds() {
        // Recalcular bounds después de remove (costoso, pero necesario)
        if (tree_.size() == 0) {
            has_keys_.store(false, std::memory_order_release);
            min_key_.store(std::numeric_limits<Key>::max(), std::memory_order_relaxed);
            max_key_.store(std::numeric_limits<Key>::min(), std::memory_order_relaxed);
        } else {
            min_key_.store(tree_.minKey(), std::memory_order_relaxed);
            max_key_.store(tree_.maxKey(), std::memory_order_relaxed);
        }
    }

public:
    TreeShard() = default;

    // Operaciones básicas con locking

    void insert(const Key& key, const Value& value) {
        std::lock_guard lock(mutex_);

        size_t old_size = tree_.size();
        tree_.insert(key, value);
        size_t new_size = tree_.size();

        if (new_size > old_size) {
            size_.fetch_add(1, std::memory_order_relaxed);
            update_bounds(key);
        }

        insert_count_.fetch_add(1, std::memory_order_relaxed);
    }

    bool remove(const Key& key) {
        std::lock_guard lock(mutex_);

        size_t old_size = tree_.size();
        tree_.remove(key);
        size_t new_size = tree_.size();

        if (new_size < old_size) {
            size_.fetch_sub(1, std::memory_order_relaxed);
            remove_count_.fetch_add(1, std::memory_order_relaxed);

            // Si la key removida era min o max, recalcular bounds
            Key current_min = min_key_.load(std::memory_order_relaxed);
            Key current_max = max_key_.load(std::memory_order_relaxed);

            if (key == current_min || key == current_max) {
                recompute_bounds();
            }

            return true;
        }

        return false;
    }

    bool contains(const Key& key) const {
        std::lock_guard lock(mutex_);
        lookup_count_.fetch_add(1, std::memory_order_relaxed);
        return tree_.contains(key);
    }

    std::optional<Value> get(const Key& key) const {
        std::lock_guard lock(mutex_);
        lookup_count_.fetch_add(1, std::memory_order_relaxed);

        if (tree_.contains(key)) {
            return tree_.get(key);
        }

        return std::nullopt;
    }

    // Lock-free reads de estadísticas
    size_t size() const {
        return size_.load(std::memory_order_relaxed);
    }

    size_t insert_count() const {
        return insert_count_.load(std::memory_order_relaxed);
    }

    size_t remove_count() const {
        return remove_count_.load(std::memory_order_relaxed);
    }

    size_t lookup_count() const {
        return lookup_count_.load(std::memory_order_relaxed);
    }

    // Range query optimization: verificar si este shard intersecta el rango
    bool intersects_range(const Key& lo, const Key& hi) const {
        if (!has_keys_.load(std::memory_order_acquire)) {
            return false;
        }

        Key shard_min = min_key_.load(std::memory_order_relaxed);
        Key shard_max = max_key_.load(std::memory_order_relaxed);

        // Intersección: [shard_min, shard_max] ∩ [lo, hi] ≠ ∅
        return !(shard_max < lo || shard_min > hi);
    }

    // Range query: collect keys in range [lo, hi]
    template<typename OutputIt>
    void range_query(const Key& lo, const Key& hi, OutputIt out) const {
        std::lock_guard lock(mutex_);
        range_query_recursive(tree_.getRoot(), lo, hi, out);
    }

private:
    template<typename OutputIt>
    void range_query_recursive(typename AVLTree<Key, Value>::Node* node,
                               const Key& lo,
                               const Key& hi,
                               OutputIt out) const {
        if (!node) return;

        // In-order traversal con pruning
        if (node->key > lo) {
            range_query_recursive(node->left, lo, hi, out);
        }

        if (node->key >= lo && node->key <= hi) {
            *out++ = std::make_pair(node->key, node->value);
        }

        if (node->key < hi) {
            range_query_recursive(node->right, lo, hi, out);
        }
    }

public:
    // Estadísticas completas
    struct Stats {
        size_t size;
        size_t inserts;
        size_t removes;
        size_t lookups;
        std::optional<Key> min_key;
        std::optional<Key> max_key;
    };

    Stats get_stats() const {
        Stats stats;
        stats.size = size_.load(std::memory_order_relaxed);
        stats.inserts = insert_count_.load(std::memory_order_relaxed);
        stats.removes = remove_count_.load(std::memory_order_relaxed);
        stats.lookups = lookup_count_.load(std::memory_order_relaxed);

        if (has_keys_.load(std::memory_order_acquire)) {
            stats.min_key = min_key_.load(std::memory_order_relaxed);
            stats.max_key = max_key_.load(std::memory_order_relaxed);
        }

        return stats;
    }

    // Clear (para testing)
    void clear() {
        std::lock_guard lock(mutex_);
        // No podemos llamar clear() directamente, reconstruir el tree
        tree_ = AVLTree<Key, Value>();
        size_.store(0, std::memory_order_relaxed);
        insert_count_.store(0, std::memory_order_relaxed);
        remove_count_.store(0, std::memory_order_relaxed);
        lookup_count_.store(0, std::memory_order_relaxed);
        has_keys_.store(false, std::memory_order_release);
    }
};

#endif // SHARD_HPP
