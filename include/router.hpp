#ifndef ROUTER_HPP
#define ROUTER_HPP

#include <vector>
#include <atomic>
#include <mutex>
#include <functional>
#include <random>
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <chrono>

// Adversary-Resistant Router
// Protege contra targeted attacks mediante:
// 1. Rate limiting por patrón de keys
// 2. Exponential backoff en redirecciones consecutivas
// 3. Detección de patrones sospechosos

template<typename Key>
class AdversaryResistantRouter {
public:
    enum class Strategy {
        STATIC_HASH,      // Baseline: simple hash
        LOAD_AWARE,       // Detecta y redistribuye hotspots
        CONSISTENT_HASH,  // Virtual nodes
        VIRTUAL_NODES,    // Alias for CONSISTENT_HASH (for compatibility)
        INTELLIGENT       // Híbrido adaptativo
    };

private:
    size_t num_shards_;
    Strategy strategy_;

    // Load tracking
    std::vector<std::atomic<size_t>> shard_loads_;
    std::vector<std::atomic<size_t>> recent_inserts_;  // Ventana reciente

    // Virtual nodes para consistent hashing
    struct VirtualNode {
        size_t shard_id;
        size_t hash_value;

        bool operator<(const VirtualNode& other) const {
            return hash_value < other.hash_value;
        }
    };
    std::vector<VirtualNode> virtual_nodes_;

    // Adversary resistance: tracking de redirecciones consecutivas
    struct RedirectHistory {
        size_t consecutive_redirects = 0;
        std::chrono::steady_clock::time_point last_redirect;
    };
    std::unordered_map<Key, RedirectHistory> redirect_history_;
    mutable std::mutex history_mutex_;

    // Detección de ataques
    std::atomic<size_t> suspicious_patterns_{0};
    std::atomic<size_t> blocked_redirects_{0};

    // Cache para Intelligent strategy (evita llamar get_stats() en cada routing)
    mutable std::atomic<size_t> ops_since_cache_update_{0};
    mutable std::atomic<bool> cached_has_hotspot_{false};
    mutable std::atomic<double> cached_balance_score_{1.0};
    mutable std::atomic<size_t> adaptive_interval_{10};  // Intervalo adaptativo
    static constexpr size_t MIN_CACHE_INTERVAL = 10;     // Mínimo: reactivo
    static constexpr size_t MAX_CACHE_INTERVAL = 500;    // Máximo: eficiente

    // Configuración
    static constexpr size_t VNODES_PER_SHARD = 16;
    static constexpr size_t WINDOW_SIZE = 50;
    static constexpr double HOTSPOT_THRESHOLD = 1.5;
    static constexpr size_t MAX_CONSECUTIVE_REDIRECTS = 3;
    static constexpr auto REDIRECT_COOLDOWN = std::chrono::milliseconds(100);

    // Random para desempate
    mutable std::mt19937 rng_;
    std::mutex rng_mutex_;

    // Hash functions
    size_t hash1(const Key& key) const {
        return std::hash<Key>{}(key);
    }

    size_t hash2(const Key& key) const {
        size_t h = std::hash<Key>{}(key);
        h ^= (h >> 16);
        h *= 0x85ebca6b;
        h ^= (h >> 13);
        h *= 0xc2b2ae35;
        h ^= (h >> 16);
        return h;
    }

    // Inicializar virtual nodes
    void init_virtual_nodes() {
        virtual_nodes_.clear();
        virtual_nodes_.reserve(num_shards_ * VNODES_PER_SHARD);

        for (size_t shard = 0; shard < num_shards_; ++shard) {
            for (size_t vnode = 0; vnode < VNODES_PER_SHARD; ++vnode) {
                size_t hash = hash2(shard * VNODES_PER_SHARD + vnode);
                virtual_nodes_.push_back({shard, hash});
            }
        }

        std::sort(virtual_nodes_.begin(), virtual_nodes_.end());
    }

    // Verificar si una redirección es sospechosa
    bool is_redirect_suspicious(const Key& key, size_t natural_shard, size_t target_shard) {
        if (natural_shard == target_shard) {
            return false;  // No es redirección
        }

        std::lock_guard lock(history_mutex_);
        auto& history = redirect_history_[key];
        auto now = std::chrono::steady_clock::now();

        // Verificar cooldown
        if (history.consecutive_redirects > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - history.last_redirect
            );

            if (elapsed < REDIRECT_COOLDOWN) {
                // Dentro del cooldown: incrementar contador
                history.consecutive_redirects++;

                if (history.consecutive_redirects > MAX_CONSECUTIVE_REDIRECTS) {
                    // SOSPECHOSO: demasiadas redirecciones consecutivas
                    suspicious_patterns_.fetch_add(1, std::memory_order_relaxed);
                    blocked_redirects_.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }
            } else {
                // Cooldown expirado: resetear
                history.consecutive_redirects = 1;
            }
        } else {
            history.consecutive_redirects = 1;
        }

        history.last_redirect = now;
        return false;
    }

public:
    explicit AdversaryResistantRouter(size_t num_shards, Strategy strategy = Strategy::INTELLIGENT)
        : num_shards_(num_shards)
        , strategy_(strategy)
        , shard_loads_(num_shards)
        , recent_inserts_(num_shards)
        , rng_(std::random_device{}())
    {
        for (size_t i = 0; i < num_shards_; ++i) {
            shard_loads_[i] = 0;
            recent_inserts_[i] = 0;
        }

        if (strategy_ == Strategy::CONSISTENT_HASH || strategy_ == Strategy::INTELLIGENT) {
            init_virtual_nodes();
        }
    }

    // Routing principal con protección adversarial
    size_t route(const Key& key) {
        size_t natural_shard = route_static_hash(key);

        // Si la estrategia es static hash, retornar directamente
        if (strategy_ == Strategy::STATIC_HASH) {
            return natural_shard;
        }

        // Determinar shard target según estrategia
        size_t target_shard;

        switch (strategy_) {
            case Strategy::LOAD_AWARE:
                target_shard = route_load_aware(key, natural_shard);
                break;

            case Strategy::CONSISTENT_HASH:
            case Strategy::VIRTUAL_NODES:
                target_shard = route_consistent_hash(key);
                break;

            case Strategy::INTELLIGENT:
                target_shard = route_intelligent(key, natural_shard);
                break;

            default:
                target_shard = natural_shard;
        }

        // Verificar si la redirección es sospechosa
        if (target_shard != natural_shard) {
            if (is_redirect_suspicious(key, natural_shard, target_shard)) {
                // BLOQUEADO: usar natural shard en lugar de redirect
                return natural_shard;
            }
        }

        return target_shard;
    }

    // Registrar inserción
    void record_insertion(size_t shard_idx) {
        shard_loads_[shard_idx].fetch_add(1, std::memory_order_relaxed);
        recent_inserts_[shard_idx].fetch_add(1, std::memory_order_relaxed);

        // Resetear ventana periódicamente
        size_t total_recent = 0;
        for (size_t i = 0; i < num_shards_; ++i) {
            total_recent += recent_inserts_[i].load(std::memory_order_relaxed);
        }

        if (total_recent >= WINDOW_SIZE * num_shards_) {
            for (size_t i = 0; i < num_shards_; ++i) {
                recent_inserts_[i].store(0, std::memory_order_relaxed);
            }

            // Limpiar history antigua
            std::lock_guard lock(history_mutex_);
            auto now = std::chrono::steady_clock::now();
            for (auto it = redirect_history_.begin(); it != redirect_history_.end(); ) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    now - it->second.last_redirect
                );

                if (elapsed > std::chrono::seconds(60)) {
                    it = redirect_history_.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    void record_removal(size_t shard_idx) {
        if (shard_loads_[shard_idx].load(std::memory_order_relaxed) > 0) {
            shard_loads_[shard_idx].fetch_sub(1, std::memory_order_relaxed);
        }
    }

    // Estadísticas
    struct Stats {
        size_t total_load;
        size_t min_load;
        size_t max_load;
        double avg_load;
        double balance_score;
        bool has_hotspot;
        size_t suspicious_patterns;
        size_t blocked_redirects;
    };

    Stats get_stats() const {
        Stats stats{};
        stats.total_load = 0;
        stats.min_load = SIZE_MAX;
        stats.max_load = 0;

        for (size_t i = 0; i < num_shards_; ++i) {
            size_t load = shard_loads_[i].load(std::memory_order_relaxed);
            stats.total_load += load;
            stats.min_load = std::min(stats.min_load, load);
            stats.max_load = std::max(stats.max_load, load);
        }

        stats.avg_load = stats.total_load / static_cast<double>(num_shards_);

        // Balance score
        if (stats.avg_load > 0) {
            double variance = 0;
            for (size_t i = 0; i < num_shards_; ++i) {
                double diff = shard_loads_[i].load(std::memory_order_relaxed) - stats.avg_load;
                variance += diff * diff;
            }
            variance /= num_shards_;
            double std_dev = std::sqrt(variance);
            stats.balance_score = std::max(0.0, 1.0 - (std_dev / stats.avg_load));
        } else {
            stats.balance_score = 1.0;
        }

        stats.has_hotspot = (stats.max_load > HOTSPOT_THRESHOLD * stats.avg_load);
        stats.suspicious_patterns = suspicious_patterns_.load(std::memory_order_relaxed);
        stats.blocked_redirects = blocked_redirects_.load(std::memory_order_relaxed);

        return stats;
    }

private:
    // Hash robusto (Murmur3 finalizer) - consistente entre compiladores
    size_t robust_hash(const Key& key) const {
        size_t h = std::hash<Key>{}(key);
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ULL;
        h ^= h >> 33;
        return h;
    }

    // Hash débil (identity) - SOLO PARA TESTING de balanceo bajo ataque
    // Permite ataques triviales: keys múltiplos de num_shards van al shard 0
    size_t weak_hash(const Key& key) const {
        return std::hash<Key>{}(key);  // Identity en GCC, scrambled en ICX
    }

    size_t route_static_hash(const Key& key) const {
        return robust_hash(key) % num_shards_;
    }

    size_t route_load_aware(const Key& key, size_t natural_shard) {
        size_t primary_load = shard_loads_[natural_shard].load(std::memory_order_relaxed);

        // Calcular carga promedio
        size_t total_load = 0;
        for (size_t i = 0; i < num_shards_; ++i) {
            total_load += shard_loads_[i].load(std::memory_order_relaxed);
        }
        double avg_load = total_load / static_cast<double>(num_shards_);

        // Si sobrecargado, buscar alternativa
        if (primary_load > HOTSPOT_THRESHOLD * avg_load) {
            size_t best_shard = 0;
            size_t min_load = shard_loads_[0].load(std::memory_order_relaxed);

            for (size_t i = 1; i < num_shards_; ++i) {
                size_t load = shard_loads_[i].load(std::memory_order_relaxed);
                if (load < min_load) {
                    best_shard = i;
                    min_load = load;
                }
            }

            if (min_load < avg_load) {
                return best_shard;
            }

            // Fallback: random
            std::lock_guard lock(rng_mutex_);
            return rng_() % num_shards_;
        }

        return natural_shard;
    }

    size_t route_consistent_hash(const Key& key) const {
        size_t key_hash = hash1(key);

        // Búsqueda binaria
        auto it = std::lower_bound(
            virtual_nodes_.begin(),
            virtual_nodes_.end(),
            key_hash,
            [](const VirtualNode& vnode, size_t hash) {
                return vnode.hash_value < hash;
            }
        );

        if (it == virtual_nodes_.end()) {
            it = virtual_nodes_.begin();
        }

        return it->shard_id;
    }

    size_t route_intelligent(const Key& key, size_t natural_shard) {
        // Fast path: si está estable, usar static hash directamente (mismo costo que STATIC_HASH)
        size_t interval = adaptive_interval_.load(std::memory_order_relaxed);
        if (interval >= MAX_CACHE_INTERVAL) {
            // Sistema estable - bypasear todo el overhead
            return natural_shard;
        }

        // Actualizar cache periódicamente
        size_t ops = ops_since_cache_update_.fetch_add(1, std::memory_order_relaxed);
        if (ops >= interval) {
            ops_since_cache_update_.store(0, std::memory_order_relaxed);
            update_stats_cache();
        }

        // Usar valores cacheados
        bool has_hotspot = cached_has_hotspot_.load(std::memory_order_relaxed);
        double balance = cached_balance_score_.load(std::memory_order_relaxed);

        // Si hay hotspot o desbalance, usar load-aware
        if (has_hotspot || balance < 0.9) {
            return route_load_aware(key, natural_shard);
        }

        // Balanceado - usar natural shard (rápido)
        return natural_shard;
    }

    void update_stats_cache() const {
        size_t total = 0, min_load = SIZE_MAX, max_load = 0;
        
        for (size_t i = 0; i < num_shards_; ++i) {
            size_t load = shard_loads_[i].load(std::memory_order_relaxed);
            total += load;
            min_load = std::min(min_load, load);
            max_load = std::max(max_load, load);
        }

        double avg = total / static_cast<double>(num_shards_);
        
        // Balance score simplificado
        double balance = (avg > 0) ? std::max(0.0, 1.0 - static_cast<double>(max_load - min_load) / (2.0 * avg)) : 1.0;
        bool hotspot = (max_load > HOTSPOT_THRESHOLD * avg);
        
        cached_balance_score_.store(balance, std::memory_order_relaxed);
        cached_has_hotspot_.store(hotspot, std::memory_order_relaxed);
        
        // Adaptar intervalo: más frecuente si hay problemas, menos si está estable
        size_t new_interval;
        if (hotspot || balance < 0.8) {
            new_interval = MIN_CACHE_INTERVAL;  // Reactivo bajo ataque
        } else if (balance > 0.95) {
            new_interval = MAX_CACHE_INTERVAL;  // Eficiente cuando estable
        } else {
            new_interval = MIN_CACHE_INTERVAL + 
                static_cast<size_t>((balance - 0.8) * (MAX_CACHE_INTERVAL - MIN_CACHE_INTERVAL) / 0.15);
        }
        adaptive_interval_.store(new_interval, std::memory_order_relaxed);
    }
};

#endif // ROUTER_HPP
