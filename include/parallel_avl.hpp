#ifndef PARALLEL_AVL_HPP
#define PARALLEL_AVL_HPP

// =============================================================================
// ParallelAVL - Árbol AVL Paralelo con Escalado Dinámico
// =============================================================================
//
// Características:
//   - Sharding automático con N árboles AVL independientes
//   - Router adversario-resistente con detección de hotspots
//   - Escalado dinámico: add_shard(), remove_shard(), force_rebalance()
//   - Garantía de linearizabilidad via RedirectIndex
//   - Range queries optimizadas
//
// Estrategias de routing:
//   - STATIC_HASH:    Hash simple, máximo rendimiento
//   - LOAD_AWARE:     Redistribuye bajo carga desbalanceada  
//   - INTELLIGENT:    Adaptativo con detección de ataques (default)
//
// Uso:
//   ParallelAVL<int, string> tree(8);  // 8 shards
//   tree.insert(42, "value");
//   tree.contains(42);  // true
//   tree.add_shard();   // Escalar a 9 shards
//
// =============================================================================

#include "shard.hpp"
#include "router.hpp"
#include "redirect_index.hpp"
#include <vector>
#include <memory>
#include <optional>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <cmath>

template<typename Key, typename Value>
class ParallelAVL {
    static_assert(std::is_default_constructible_v<Key>, "Key must be default constructible");
    static_assert(std::is_copy_constructible_v<Key>, "Key must be copy constructible");

public:
    // Public type aliases
    using Shard = TreeShard<Key, Value>;
    using Router = AdversaryResistantRouter<Key>;
    using RouterStrategy = typename Router::Strategy;

private:
    // Hash robusto (Murmur3 finalizer) - consistente entre compiladores
    static size_t robust_hash(const Key& key) {
        size_t h = std::hash<Key>{}(key);
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ULL;
        h ^= h >> 33;
        return h;
    }

    size_t num_shards_;
    std::vector<std::unique_ptr<Shard>> shards_;
    std::unique_ptr<Router> router_;
    std::unique_ptr<RedirectIndex<Key>> redirect_index_;

    // Estadísticas globales (mutable para actualizar en métodos const)
    mutable std::atomic<size_t> total_ops_{0};
    mutable std::atomic<size_t> redirect_index_hits_{0};
    
    // Flags de optimización
    std::atomic<bool> topology_changed_{false};  // Necesita búsqueda exhaustiva
    std::atomic<bool> has_redirects_{false};     // Fast-path: evitar redirect_index si vacío

public:
    explicit ParallelAVL(
        size_t num_shards = 8,
        RouterStrategy strategy = RouterStrategy::INTELLIGENT
    ) : num_shards_(num_shards)
    {
        // Crear shards
        shards_.reserve(num_shards_);
        for (size_t i = 0; i < num_shards_; ++i) {
            shards_.push_back(std::make_unique<Shard>());
        }

        // Crear router
        router_ = std::make_unique<Router>(num_shards_, strategy);

        // Crear redirect index
        redirect_index_ = std::make_unique<RedirectIndex<Key>>();
    }

    // Insert con linearizabilidad
    void insert(const Key& key, const Value& value) {
        total_ops_.fetch_add(1, std::memory_order_relaxed);

        // Determinar shard via router (puede haber redirección)
        size_t natural_shard = robust_hash(key) % num_shards_;
        size_t target_shard = router_->route(key);

        // Capturar tamaño antes de insertar
        size_t old_size = shards_[target_shard]->size();
        
        // Insertar en el shard target
        shards_[target_shard]->insert(key, value);
        
        // Solo notificar al router si realmente se insertó una key nueva
        // FIX: Evita inflar shard_loads_ con duplicados
        size_t new_size = shards_[target_shard]->size();
        if (new_size > old_size) {
            router_->record_insertion(target_shard);
            
            // Si hubo redirección, registrar en el índice
            if (target_shard != natural_shard) {
                redirect_index_->record_redirect(key, natural_shard, target_shard);
                has_redirects_.store(true, std::memory_order_relaxed);
            }
        }
    }

    // Contains con linearizabilidad garantizada - OPTIMIZADO
    bool contains(const Key& key) const {
        // Fast path: buscar en shard natural (caso común)
        size_t natural_shard = robust_hash(key) % num_shards_;
        
        if (shards_[natural_shard]->contains(key)) {
            return true;
        }

        // Solo continuar si hay posibilidad de que esté en otro lugar
        if (!has_redirects_.load(std::memory_order_relaxed) && 
            !topology_changed_.load(std::memory_order_relaxed)) {
            return false;  // Fast exit: no hay redirects ni cambios de topología
        }

        total_ops_.fetch_add(1, std::memory_order_relaxed);

        // Consultar redirect index
        if (has_redirects_.load(std::memory_order_relaxed)) {
            auto redirected_shard = redirect_index_->lookup(key);
            if (redirected_shard.has_value()) {
                redirect_index_hits_.fetch_add(1, std::memory_order_relaxed);
                return shards_[*redirected_shard]->contains(key);
            }
        }

        // Búsqueda exhaustiva solo si hubo cambio de topología
        if (topology_changed_.load(std::memory_order_acquire)) {
            for (size_t i = 0; i < num_shards_; ++i) {
                if (i == natural_shard) continue;
                if (shards_[i]->contains(key)) {
                    return true;
                }
            }
        }

        return false;
    }

    // Get con linearizabilidad - OPTIMIZADO
    std::optional<Value> get(const Key& key) const {
        // Fast path: buscar en shard natural
        size_t natural_shard = robust_hash(key) % num_shards_;
        auto result = shards_[natural_shard]->get(key);

        if (result.has_value()) {
            return result;
        }

        // Fast exit si no hay redirects ni cambios
        if (!has_redirects_.load(std::memory_order_relaxed) && 
            !topology_changed_.load(std::memory_order_relaxed)) {
            return std::nullopt;
        }

        total_ops_.fetch_add(1, std::memory_order_relaxed);

        // Consultar redirect index
        if (has_redirects_.load(std::memory_order_relaxed)) {
            auto redirected_shard = redirect_index_->lookup(key);
            if (redirected_shard.has_value()) {
                redirect_index_hits_.fetch_add(1, std::memory_order_relaxed);
                return shards_[*redirected_shard]->get(key);
            }
        }

        // Búsqueda exhaustiva solo si hubo cambio de topología
        if (topology_changed_.load(std::memory_order_acquire)) {
            for (size_t i = 0; i < num_shards_; ++i) {
                if (i == natural_shard) continue;
                auto res = shards_[i]->get(key);
                if (res.has_value()) {
                    return res;
                }
            }
        }

        return std::nullopt;
    }

    // Remove
    bool remove(const Key& key) {
        total_ops_.fetch_add(1, std::memory_order_relaxed);

        // Buscar en shard natural primero
        size_t natural_shard = robust_hash(key) % num_shards_;

        if (shards_[natural_shard]->remove(key)) {
            router_->record_removal(natural_shard);
            redirect_index_->remove(key);  // Limpiar del índice si estaba
            return true;
        }

        // Buscar en shard redirigido
        auto redirected_shard = redirect_index_->lookup(key);

        if (redirected_shard.has_value()) {
            if (shards_[*redirected_shard]->remove(key)) {
                router_->record_removal(*redirected_shard);
                redirect_index_->remove(key);
                return true;
            }
        }

        // Si hubo cambio de topología, buscar en todos los shards
        if (topology_changed_.load(std::memory_order_acquire)) {
            for (size_t i = 0; i < num_shards_; ++i) {
                if (i == natural_shard) continue;
                if (shards_[i]->remove(key)) {
                    router_->record_removal(i);
                    return true;
                }
            }
        }

        return false;
    }

    // Range query eficiente: solo toca shards que intersectan el rango
    template<typename OutputIt>
    void range_query(const Key& lo, const Key& hi, OutputIt out) const {
        total_ops_.fetch_add(1, std::memory_order_relaxed);

        std::vector<std::pair<Key, Value>> results;

        // Solo consultar shards que intersectan el rango
        for (size_t i = 0; i < num_shards_; ++i) {
            if (shards_[i]->intersects_range(lo, hi)) {
                std::vector<std::pair<Key, Value>> shard_results;
                shards_[i]->range_query(lo, hi, std::back_inserter(shard_results));
                results.insert(results.end(), shard_results.begin(), shard_results.end());
            }
        }

        // Ordenar resultados
        std::sort(results.begin(), results.end(),
                 [](const auto& a, const auto& b) { return a.first < b.first; });

        // Output
        std::copy(results.begin(), results.end(), out);
    }

    // Size total
    size_t size() const {
        size_t total = 0;
        for (const auto& shard : shards_) {
            total += shard->size();
        }
        return total;
    }

    // Estadísticas completas
    struct Stats {
        size_t num_shards;
        size_t total_size;
        size_t total_ops;

        // Per-shard stats
        std::vector<size_t> shard_sizes;
        std::vector<size_t> shard_inserts;
        std::vector<size_t> shard_lookups;

        // Router stats
        double balance_score;
        bool has_hotspot;
        size_t suspicious_patterns;
        size_t blocked_redirects;

        // Redirect index stats
        size_t redirect_index_size;
        size_t redirect_index_hits;
        double redirect_hit_rate;

        // Memory overhead
        size_t redirect_index_memory_bytes;
    };

    Stats get_stats() const {
        Stats stats{};
        stats.num_shards = num_shards_;
        stats.total_size = 0;
        stats.total_ops = total_ops_.load(std::memory_order_relaxed);

        // Per-shard stats
        for (const auto& shard : shards_) {
            auto shard_stats = shard->get_stats();
            stats.total_size += shard_stats.size;
            stats.shard_sizes.push_back(shard_stats.size);
            stats.shard_inserts.push_back(shard_stats.inserts);
            stats.shard_lookups.push_back(shard_stats.lookups);
        }

        // Router stats
        auto router_stats = router_->get_stats();
        stats.balance_score = router_stats.balance_score;
        stats.has_hotspot = router_stats.has_hotspot;
        stats.suspicious_patterns = router_stats.suspicious_patterns;
        stats.blocked_redirects = router_stats.blocked_redirects;

        // Redirect index stats
        auto index_stats = redirect_index_->get_stats();
        stats.redirect_index_size = index_stats.index_size;
        stats.redirect_index_hits = redirect_index_hits_.load(std::memory_order_relaxed);

        size_t total_lookups = stats.total_ops;
        stats.redirect_hit_rate = total_lookups > 0 ?
            (stats.redirect_index_hits * 100.0 / total_lookups) : 0.0;

        stats.redirect_index_memory_bytes = redirect_index_->memory_bytes();

        return stats;
    }

    // Print estadísticas
    void print_stats() const {
        auto stats = get_stats();

        std::cout << "\n╔════════════════════════════════════════════╗" << std::endl;
        std::cout << "║  Parallel AVL Statistics                   ║" << std::endl;
        std::cout << "╚════════════════════════════════════════════╝\n" << std::endl;

        std::cout << "Shards: " << stats.num_shards << std::endl;
        std::cout << "Total elements: " << stats.total_size << std::endl;
        std::cout << "Total operations: " << stats.total_ops << std::endl;
        std::cout << "Balance score: " << std::fixed << std::setprecision(2)
                  << (stats.balance_score * 100) << "%" << std::endl;

        if (stats.has_hotspot) {
            std::cout << "⚠️  Hotspot detected!" << std::endl;
        }

        if (stats.suspicious_patterns > 0) {
            std::cout << "🚨 Suspicious patterns: " << stats.suspicious_patterns << std::endl;
            std::cout << "🛡️  Blocked redirects: " << stats.blocked_redirects << std::endl;
        }

        std::cout << "\nRedirect Index:" << std::endl;
        std::cout << "  Size: " << stats.redirect_index_size << " entries" << std::endl;
        std::cout << "  Hits: " << stats.redirect_index_hits << std::endl;
        std::cout << "  Hit rate: " << std::fixed << std::setprecision(2)
                  << stats.redirect_hit_rate << "%" << std::endl;
        std::cout << "  Memory: " << (stats.redirect_index_memory_bytes / 1024.0) << " KB" << std::endl;

        std::cout << "\nShard Distribution:" << std::endl;
        for (size_t i = 0; i < stats.num_shards; ++i) {
            double pct = stats.total_size > 0 ?
                        (stats.shard_sizes[i] * 100.0 / stats.total_size) : 0;

            std::cout << "  Shard " << i << ": "
                      << std::setw(6) << stats.shard_sizes[i] << " elements ("
                      << std::fixed << std::setprecision(1) << std::setw(5) << pct << "%)"
                      << " | " << stats.shard_inserts[i] << " inserts"
                      << " | " << stats.shard_lookups[i] << " lookups"
                      << std::endl;
        }

        std::cout << std::endl;
    }

    // Clear (para testing)
    void clear() {
        for (auto& shard : shards_) {
            shard->clear();
        }
        redirect_index_->clear();
        total_ops_.store(0, std::memory_order_relaxed);
        redirect_index_hits_.store(0, std::memory_order_relaxed);
    }

    // =========================================================================
    // Dynamic Scaling - Integrado desde DynamicShardedTree
    // =========================================================================

    // Agregar un nuevo shard al sistema
    // Las keys existentes permanecen donde están, nuevas inserciones
    // se distribuyen entre todos los shards (incluyendo el nuevo)
    void add_shard() {
        // Crear nuevo shard
        shards_.push_back(std::make_unique<Shard>());
        num_shards_++;

        // Recrear router con nuevo número de shards
        auto strategy = router_->get_stats().balance_score > 0.9 
            ? RouterStrategy::INTELLIGENT 
            : RouterStrategy::LOAD_AWARE;
        router_ = std::make_unique<Router>(num_shards_, strategy);

        // Marcar que hubo cambio de topología para habilitar búsqueda exhaustiva
        topology_changed_.store(true, std::memory_order_release);
    }

    // Eliminar el último shard y redistribuir sus elementos
    void remove_shard() {
        if (num_shards_ <= 1) return;

        size_t removing_id = num_shards_ - 1;

        // Extraer todos los elementos del shard a eliminar
        std::vector<std::pair<Key, Value>> to_redistribute;
        extract_shard_data(removing_id, to_redistribute);

        // Eliminar el shard
        shards_.pop_back();
        num_shards_--;

        // Recrear router
        router_ = std::make_unique<Router>(num_shards_, RouterStrategy::INTELLIGENT);

        // Marcar cambio de topología para búsqueda exhaustiva
        topology_changed_.store(true, std::memory_order_release);

        // Limpiar redirect entries que apuntaban al shard eliminado
        redirect_index_->gc_expired([removing_id](const Key&) {
            return removing_id;  // Forzar eliminación de entries obsoletas
        });

        // Re-insertar datos del shard eliminado (van a sus nuevos shards naturales)
        for (const auto& [key, value] : to_redistribute) {
            size_t target = router_->route(key);
            shards_[target]->insert(key, value);
            router_->record_insertion(target);
        }
    }

    // Forzar rebalanceo: redistribuir TODAS las keys según routing actual
    // Útil después de agregar/quitar varios shards o detectar desbalance severo
    void force_rebalance() {
        // Paso 1: Extraer todos los datos
        std::vector<std::pair<Key, Value>> all_data;
        for (size_t i = 0; i < num_shards_; ++i) {
            extract_shard_data(i, all_data);
        }

        // Paso 2: Limpiar todos los shards y redirect index
        for (auto& shard : shards_) {
            shard->clear();
        }
        redirect_index_->clear();

        // Paso 3: Recrear router fresco para routing consistente
        router_ = std::make_unique<Router>(num_shards_, RouterStrategy::STATIC_HASH);

        // Paso 4: Re-insertar todo usando hash simple (sin redirecciones)
        for (const auto& [key, value] : all_data) {
            size_t target = robust_hash(key) % num_shards_;
            shards_[target]->insert(key, value);
            router_->record_insertion(target);
        }

        // Reset stats y flag de topología - ahora todo está en su shard natural
        total_ops_.store(all_data.size(), std::memory_order_relaxed);
        redirect_index_hits_.store(0, std::memory_order_relaxed);
        topology_changed_.store(false, std::memory_order_release);
    }

    // Obtener número de shards
    size_t get_num_shards() const {
        return num_shards_;
    }

    // Balance score (0.0 - 1.0, mayor es mejor)
    double get_balance_score() const {
        return router_->get_stats().balance_score;
    }

private:
    // Helper: extraer datos de un shard específico
    void extract_shard_data(size_t shard_id, std::vector<std::pair<Key, Value>>& out) {
        shards_[shard_id]->extract_all(std::back_inserter(out));
    }
};

#endif // PARALLEL_AVL_HPP
