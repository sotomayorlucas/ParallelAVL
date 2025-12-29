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
    size_t route_static_hash(const Key& key) const {
        return hash1(key) % num_shards_;
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
        auto stats = get_stats();

        // Si hay hotspot o desbalance, usar load-aware
        if (stats.has_hotspot || stats.balance_score < 0.8) {
            return route_load_aware(key, natural_shard);
        }

        // Si está balanceado, usar consistent hashing
        return route_consistent_hash(key);
    }
};

#endif // ROUTER_HPP
