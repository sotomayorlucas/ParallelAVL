#ifndef ADAPTIVE_ROUTER_H
#define ADAPTIVE_ROUTER_H

#include <vector>
#include <atomic>
#include <mutex>
#include <functional>
#include <random>
#include <cmath>

// Sistema de Routing Dinámico Adaptativo
// Previene targeted attacks redistribuyendo automáticamente

template<typename Key>
class AdaptiveRouter {
public:
    enum class Strategy {
        STATIC_HASH,      // Hash estático (vulnerable a attacks)
        LOAD_AWARE,       // Consciente de carga (previene hotspots)
        VIRTUAL_NODES,    // Consistent hashing (distribuye mejor)
        INTELLIGENT       // Híbrido adaptativo (mejor de ambos)
    };

private:
    size_t num_shards_;
    Strategy strategy_;

    // Tracking de carga por shard (atomic para thread-safety)
    std::vector<std::atomic<size_t>> shard_loads_;
    std::vector<std::atomic<size_t>> recent_inserts_;  // Ventana reciente

    // Virtual nodes para consistent hashing
    struct VirtualNode {
        size_t shard_id;
        size_t hash_value;
    };
    std::vector<VirtualNode> virtual_nodes_;

    // Configuración
    const size_t VNODES_PER_SHARD = 16;  // Más vnodes = mejor distribución
    const size_t WINDOW_SIZE = 50;        // Ventana para detectar hotspots (reducida)
    const double HOTSPOT_THRESHOLD = 1.5; // Shard recibe >1.5x promedio (más agresivo)

    // Random para desempate
    mutable std::mt19937 rng_;
    std::mutex rng_mutex_;

    // Hash functions
    size_t hash1(const Key& key) const {
        return std::hash<Key>{}(key);
    }

    size_t hash2(const Key& key) const {
        // Secondary hash para evitar colisiones
        size_t h = std::hash<Key>{}(key);
        h ^= (h >> 16);
        h *= 0x85ebca6b;
        h ^= (h >> 13);
        h *= 0xc2b2ae35;
        h ^= (h >> 16);
        return h;
    }

    // Inicializar virtual nodes
    void initVirtualNodes() {
        virtual_nodes_.clear();
        for (size_t shard = 0; shard < num_shards_; ++shard) {
            for (size_t vnode = 0; vnode < VNODES_PER_SHARD; ++vnode) {
                size_t hash = hash2(shard * VNODES_PER_SHARD + vnode);
                virtual_nodes_.push_back({shard, hash});
            }
        }
        // Ordenar por hash value para búsqueda binaria
        std::sort(virtual_nodes_.begin(), virtual_nodes_.end(),
                 [](const VirtualNode& a, const VirtualNode& b) {
                     return a.hash_value < b.hash_value;
                 });
    }

public:
    AdaptiveRouter(size_t num_shards, Strategy strategy = Strategy::INTELLIGENT)
        : num_shards_(num_shards)
        , strategy_(strategy)
        , shard_loads_(num_shards)
        , recent_inserts_(num_shards)
        , rng_(std::random_device{}())
    {
        // Inicializar contadores
        for (size_t i = 0; i < num_shards_; ++i) {
            shard_loads_[i] = 0;
            recent_inserts_[i] = 0;
        }

        if (strategy_ == Strategy::VIRTUAL_NODES ||
            strategy_ == Strategy::INTELLIGENT) {
            initVirtualNodes();
        }
    }

    // Determinar shard para una key (thread-safe)
    size_t route(const Key& key) {
        switch (strategy_) {
            case Strategy::STATIC_HASH:
                return routeStaticHash(key);

            case Strategy::LOAD_AWARE:
                return routeLoadAware(key);

            case Strategy::VIRTUAL_NODES:
                return routeVirtualNodes(key);

            case Strategy::INTELLIGENT:
                return routeIntelligent(key);

            default:
                return routeStaticHash(key);
        }
    }

    // Registrar inserción (actualizar estadísticas)
    void recordInsertion(size_t shard_idx) {
        shard_loads_[shard_idx]++;
        recent_inserts_[shard_idx]++;

        // Resetear ventana periódicamente
        size_t total_recent = 0;
        for (size_t i = 0; i < num_shards_; ++i) {
            total_recent += recent_inserts_[i].load();
        }

        if (total_recent >= WINDOW_SIZE * num_shards_) {
            for (size_t i = 0; i < num_shards_; ++i) {
                recent_inserts_[i] = 0;
            }
        }
    }

    // Registrar remoción
    void recordRemoval(size_t shard_idx) {
        if (shard_loads_[shard_idx] > 0) {
            shard_loads_[shard_idx]--;
        }
    }

    // Obtener estadísticas
    struct Stats {
        size_t total_load;
        size_t min_load;
        size_t max_load;
        double avg_load;
        double balance_score;
        bool has_hotspot;
    };

    Stats getStats() const {
        Stats stats;
        stats.total_load = 0;
        stats.min_load = SIZE_MAX;
        stats.max_load = 0;

        for (size_t i = 0; i < num_shards_; ++i) {
            size_t load = shard_loads_[i].load();
            stats.total_load += load;
            stats.min_load = std::min(stats.min_load, load);
            stats.max_load = std::max(stats.max_load, load);
        }

        stats.avg_load = stats.total_load / static_cast<double>(num_shards_);

        // Calcular balance score
        if (stats.avg_load > 0) {
            double variance = 0;
            for (size_t i = 0; i < num_shards_; ++i) {
                double diff = shard_loads_[i].load() - stats.avg_load;
                variance += diff * diff;
            }
            variance /= num_shards_;
            double std_dev = std::sqrt(variance);
            stats.balance_score = std::max(0.0, 1.0 - (std_dev / stats.avg_load));
        } else {
            stats.balance_score = 1.0;
        }

        // Detectar hotspot
        stats.has_hotspot = (stats.max_load > HOTSPOT_THRESHOLD * stats.avg_load);

        return stats;
    }

private:
    // Estrategia 1: Hash estático (baseline)
    size_t routeStaticHash(const Key& key) const {
        return hash1(key) % num_shards_;
    }

    // Estrategia 2: Load-aware routing (inteligente y agresivo)
    size_t routeLoadAware(const Key& key) {
        // Hash primario
        size_t primary = hash1(key) % num_shards_;

        // Verificar si el shard primario está sobrecargado
        size_t primary_load = shard_loads_[primary].load();

        // Calcular carga promedio
        size_t total_load = 0;
        for (size_t i = 0; i < num_shards_; ++i) {
            total_load += shard_loads_[i].load();
        }
        double avg_load = total_load / static_cast<double>(num_shards_);

        // Si el shard primario está sobrecargado, redistribuir AGRESIVAMENTE
        if (primary_load > HOTSPOT_THRESHOLD * avg_load) {
            // Buscar el shard MENOS cargado globalmente
            size_t best_shard = 0;
            size_t min_load = shard_loads_[0].load();

            for (size_t i = 1; i < num_shards_; ++i) {
                size_t load = shard_loads_[i].load();
                if (load < min_load) {
                    best_shard = i;
                    min_load = load;
                }
            }

            // Si el shard menos cargado tiene < promedio, usarlo
            if (min_load < avg_load) {
                return best_shard;
            }

            // Si todos están sobrecargados, usar round-robin con offset
            std::lock_guard<std::mutex> lock(rng_mutex_);
            return rng_() % num_shards_;
        }

        // Verificar carga reciente (ventana corta) para detectar hotspots emergentes
        size_t recent_load = recent_inserts_[primary].load();
        double recent_avg = 0;
        for (size_t i = 0; i < num_shards_; ++i) {
            recent_avg += recent_inserts_[i].load();
        }
        recent_avg /= num_shards_;

        // Si hay hotspot emergente, redistribuir
        if (recent_load > 1.5 * recent_avg && recent_avg > 5) {
            // Usar hash secundario para dispersar
            return hash2(key) % num_shards_;
        }

        return primary;
    }

    // Estrategia 3: Consistent hashing con virtual nodes
    size_t routeVirtualNodes(const Key& key) const {
        size_t key_hash = hash1(key);

        // Búsqueda binaria del virtual node más cercano
        auto it = std::lower_bound(
            virtual_nodes_.begin(),
            virtual_nodes_.end(),
            key_hash,
            [](const VirtualNode& vnode, size_t hash) {
                return vnode.hash_value < hash;
            }
        );

        // Wrap around si llegamos al final
        if (it == virtual_nodes_.end()) {
            it = virtual_nodes_.begin();
        }

        return it->shard_id;
    }

    // Estrategia 4: Híbrido inteligente (mejor de ambos mundos)
    size_t routeIntelligent(const Key& key) {
        auto stats = getStats();

        // Si hay hotspot detectado, usar load-aware
        if (stats.has_hotspot || stats.balance_score < 0.8) {
            return routeLoadAware(key);
        }

        // Si está balanceado, usar virtual nodes (mejor distribución)
        return routeVirtualNodes(key);
    }
};

#endif // ADAPTIVE_ROUTER_H
