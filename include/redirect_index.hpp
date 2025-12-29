#ifndef REDIRECT_INDEX_HPP
#define REDIRECT_INDEX_HPP

#include <unordered_map>
#include <shared_mutex>
#include <mutex>
#include <atomic>
#include <optional>
#include <functional>
#include <algorithm>

// Fix de Linearizabilidad:
// Mantiene registro de keys que fueron redirigidas del shard natural a otro
// debido a load balancing. Esto garantiza que búsquedas subsecuentes
// encuentren la key correctamente.
//
// Problema sin esto:
//   1. insert(42, val) → redirigido a shard 3 (por load balancing)
//   2. contains(42) → busca en shard hash(42)%N = 5 → NO ENCUENTRA ❌
//
// Solución con RedirectIndex:
//   1. insert(42, val) → shard 3 + registrar redirect_index[42] = 3
//   2. contains(42) → busca shard 5, no encuentra, consulta index → shard 3 ✓

template<typename Key>
class RedirectIndex {
private:
    // Mapa de key → shard_id para keys redirigidas
    std::unordered_map<Key, size_t> redirects_;

    // Shared mutex: múltiples lectores, un escritor
    mutable std::shared_mutex mutex_;

    // Estadísticas (mutable para actualizar en métodos const)
    std::atomic<size_t> total_redirects_{0};
    mutable std::atomic<size_t> lookups_{0};
    mutable std::atomic<size_t> hits_{0};

public:
    // Registrar que una key fue redirigida a un shard específico
    void record_redirect(const Key& key, size_t natural_shard, size_t actual_shard) {
        // Solo registrar si realmente hubo redirección
        if (natural_shard == actual_shard) {
            return;
        }

        std::unique_lock lock(mutex_);
        redirects_[key] = actual_shard;
        total_redirects_.fetch_add(1, std::memory_order_relaxed);
    }

    // Buscar si una key fue redirigida
    std::optional<size_t> lookup(const Key& key) const {
        lookups_.fetch_add(1, std::memory_order_relaxed);

        std::shared_lock lock(mutex_);
        auto it = redirects_.find(key);

        if (it != redirects_.end()) {
            hits_.fetch_add(1, std::memory_order_relaxed);
            return it->second;
        }

        return std::nullopt;
    }

    // Eliminar entrada cuando una key es removida
    void remove(const Key& key) {
        std::unique_lock lock(mutex_);
        redirects_.erase(key);
    }

    // Garbage Collection: Limpiar entries obsoletas
    //
    // Problema: Después de rebalancing, algunas keys pueden volver a su shard natural.
    // El redirect index tendría entries innecesarias que consumen memoria.
    //
    // Solución: Periódicamente ejecutar GC que elimina redirects donde
    // current_router(key) == actual_shard (ya no hay redirect)
    //
    // Uso: tree.get_redirect_index().gc_expired([&](const Key& k) {
    //          return router_->route(k);
    //      });
    size_t gc_expired(std::function<size_t(const Key&)> current_router) {
        std::unique_lock lock(mutex_);

        size_t removed = 0;

        // C++20: std::erase_if
        // C++17: Manual iteration
        for (auto it = redirects_.begin(); it != redirects_.end(); ) {
            const Key& key = it->first;
            size_t actual_shard = it->second;

            // Si el router actual rutearía a este mismo shard, no hay redirect
            if (current_router(key) == actual_shard) {
                it = redirects_.erase(it);
                removed++;
            } else {
                ++it;
            }
        }

        return removed;
    }

    // Limpiar el índice (útil para testing)
    void clear() {
        std::unique_lock lock(mutex_);
        redirects_.clear();
        total_redirects_.store(0, std::memory_order_relaxed);
        lookups_.store(0, std::memory_order_relaxed);
        hits_.store(0, std::memory_order_relaxed);
    }

    // Estadísticas
    struct Stats {
        size_t total_redirects;
        size_t lookups;
        size_t hits;
        double hit_rate;
        size_t index_size;
    };

    Stats get_stats() const {
        std::shared_lock lock(mutex_);

        size_t total = total_redirects_.load(std::memory_order_relaxed);
        size_t looks = lookups_.load(std::memory_order_relaxed);
        size_t h = hits_.load(std::memory_order_relaxed);

        return Stats{
            .total_redirects = total,
            .lookups = looks,
            .hits = h,
            .hit_rate = looks > 0 ? (h * 100.0 / looks) : 0.0,
            .index_size = redirects_.size()
        };
    }

    // Memory overhead
    size_t memory_bytes() const {
        std::shared_lock lock(mutex_);
        // Aproximación: cada entrada = sizeof(Key) + sizeof(size_t) + overhead del hash table
        return redirects_.size() * (sizeof(Key) + sizeof(size_t) + 16);
    }
};

#endif // REDIRECT_INDEX_HPP
