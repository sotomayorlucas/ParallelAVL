#ifndef AVL_TREE_PARALLEL_V2_H
#define AVL_TREE_PARALLEL_V2_H

#include "BaseTree.h"
#include "AVLTree.h"
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <optional>
#include <memory>
#include <functional>
#include <thread>
#include <stdexcept>
#include <iostream>
#include <iomanip>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <unordered_map>

// =============================================================================
// AVL Tree Parallel V2 - Con Trabajos Futuros Implementados
// =============================================================================
//
// Mejoras sobre V1:
// 1. shared_mutex para lecturas concurrentes (paso hacia RCU)
// 2. Heurísticas ML-lite para predicción de hotspots
// 3. Estructura preparada para shard count dinámico
// 4. Hooks para extensión distribuida
//
// =============================================================================

// -----------------------------------------------------------------------------
// Componente 1: Predictor de Hotspots (ML-lite con EMA)
// -----------------------------------------------------------------------------
template<typename Key>
class HotspotPredictor {
public:
    struct ShardMetrics {
        double ema_load;           // Exponential Moving Average de carga
        double ema_variance;       // EMA de varianza (volatilidad)
        double trend;              // Tendencia: positiva = creciendo
        size_t access_count;
        std::chrono::steady_clock::time_point last_update;
        
        ShardMetrics() 
            : ema_load(0), ema_variance(0), trend(0), access_count(0)
            , last_update(std::chrono::steady_clock::now()) {}
    };

private:
    size_t num_shards_;
    
    // Parámetros EMA (ajustables)
    double alpha_;           // Factor de suavizado (0.1 = lento, 0.3 = rápido)
    double threshold_std_;   // Umbral en desviaciones estándar
    
    std::vector<ShardMetrics> metrics_;
    mutable std::mutex mutex_;
    
    // Histórico para análisis de patrones temporales
    struct TemporalPattern {
        std::array<double, 24> hourly_load;  // Carga promedio por hora
        size_t samples;
        
        TemporalPattern() : hourly_load{}, samples(0) {}
    };
    std::vector<TemporalPattern> temporal_patterns_;

public:
    explicit HotspotPredictor(size_t num_shards, double alpha = 0.2, double threshold = 2.0)
        : num_shards_(num_shards)
        , alpha_(alpha)
        , threshold_std_(threshold)
        , metrics_(num_shards)
        , temporal_patterns_(num_shards)
    {}

    // Registrar acceso a un shard
    void record_access(size_t shard_idx, bool is_write = false) {
        if (shard_idx >= num_shards_) return;
        
        std::lock_guard<std::mutex> lock(mutex_);
        auto& m = metrics_[shard_idx];
        auto now = std::chrono::steady_clock::now();
        
        // Calcular delta time
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m.last_update
        ).count();
        
        // Actualizar contador
        m.access_count++;
        
        // Actualizar EMA de carga (accesos por segundo aproximado)
        double instant_rate = elapsed > 0 ? 1000.0 / elapsed : 100.0;
        double old_ema = m.ema_load;
        m.ema_load = alpha_ * instant_rate + (1 - alpha_) * m.ema_load;
        
        // Actualizar tendencia
        m.trend = m.ema_load - old_ema;
        
        // Actualizar varianza (EMA del error cuadrado)
        double error = instant_rate - m.ema_load;
        m.ema_variance = alpha_ * (error * error) + (1 - alpha_) * m.ema_variance;
        
        m.last_update = now;
        
        // Actualizar patrón temporal
        auto time_t = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now()
        );
        std::tm* tm = std::localtime(&time_t);
        int hour = tm->tm_hour;
        
        auto& tp = temporal_patterns_[shard_idx];
        tp.hourly_load[hour] = alpha_ * instant_rate + (1 - alpha_) * tp.hourly_load[hour];
        tp.samples++;
    }

    // Predecir si un shard será hotspot en los próximos N segundos
    struct Prediction {
        bool will_be_hotspot;
        double confidence;       // 0.0 - 1.0
        double predicted_load;
        double time_to_hotspot;  // segundos estimados
    };

    Prediction predict_hotspot(size_t shard_idx) const {
        Prediction pred{false, 0.0, 0.0, -1.0};
        if (shard_idx >= num_shards_) return pred;
        
        std::lock_guard<std::mutex> lock(mutex_);
        
        // Calcular estadísticas globales
        double total_load = 0;
        double max_load = 0;
        for (size_t i = 0; i < num_shards_; ++i) {
            total_load += metrics_[i].ema_load;
            max_load = std::max(max_load, metrics_[i].ema_load);
        }
        double avg_load = total_load / num_shards_;
        
        // Calcular desviación estándar
        double variance = 0;
        for (size_t i = 0; i < num_shards_; ++i) {
            double diff = metrics_[i].ema_load - avg_load;
            variance += diff * diff;
        }
        double std_dev = std::sqrt(variance / num_shards_);
        
        const auto& m = metrics_[shard_idx];
        pred.predicted_load = m.ema_load + m.trend * 5.0;  // Proyección 5 segundos
        
        // Umbral de hotspot
        double hotspot_threshold = avg_load + threshold_std_ * std_dev;
        if (hotspot_threshold < avg_load * 1.5) {
            hotspot_threshold = avg_load * 1.5;  // Mínimo 1.5x promedio
        }
        
        // ¿Ya es hotspot?
        if (m.ema_load > hotspot_threshold) {
            pred.will_be_hotspot = true;
            pred.confidence = std::min(1.0, (m.ema_load - hotspot_threshold) / std_dev);
            pred.time_to_hotspot = 0;
            return pred;
        }
        
        // ¿Tendencia hacia hotspot?
        if (m.trend > 0 && pred.predicted_load > hotspot_threshold) {
            pred.will_be_hotspot = true;
            
            // Estimar tiempo hasta hotspot
            double gap = hotspot_threshold - m.ema_load;
            pred.time_to_hotspot = gap / m.trend;
            
            // Confianza basada en volatilidad y tendencia
            double volatility = std::sqrt(m.ema_variance);
            pred.confidence = std::max(0.0, std::min(1.0, 
                m.trend / (volatility + 0.1) * 0.5
            ));
        }
        
        return pred;
    }

    // Obtener shard recomendado (el menos probable de ser hotspot)
    size_t get_coolest_shard() const {
        std::lock_guard<std::mutex> lock(mutex_);
        
        size_t best = 0;
        double best_score = std::numeric_limits<double>::max();
        
        for (size_t i = 0; i < num_shards_; ++i) {
            // Score = carga actual + tendencia ponderada
            double score = metrics_[i].ema_load + metrics_[i].trend * 10.0;
            if (score < best_score) {
                best_score = score;
                best = i;
            }
        }
        
        return best;
    }

    // Obtener predicción de carga por hora (para scheduling)
    double get_predicted_load_for_hour(size_t shard_idx, int hour) const {
        if (shard_idx >= num_shards_ || hour < 0 || hour >= 24) return 0;
        
        std::lock_guard<std::mutex> lock(mutex_);
        return temporal_patterns_[shard_idx].hourly_load[hour];
    }

    // Reset para testing
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        metrics_.assign(num_shards_, ShardMetrics{});
        temporal_patterns_.assign(num_shards_, TemporalPattern{});
    }
};


// -----------------------------------------------------------------------------
// Componente 2: Gestor de Shards Dinámicos
// -----------------------------------------------------------------------------
template<typename Key, typename Value>
class DynamicShardManager {
public:
    struct ShardInfo {
        size_t id;
        size_t element_count;
        bool is_migrating;
        size_t target_shard;  // Si está migrando, a dónde
    };

private:
    std::atomic<size_t> num_shards_;
    std::atomic<size_t> pending_migrations_{0};
    
    // Índice de redirección: key -> shard actual (para migración lazy)
    std::unordered_map<Key, size_t> redirect_index_;
    mutable std::shared_mutex redirect_mutex_;
    
    // Versión del esquema (incrementa en cada resize)
    std::atomic<uint64_t> schema_version_{0};
    
    // Consistent hash ring para escalado
    struct HashRingEntry {
        size_t shard_id;
        size_t hash_point;
        bool operator<(const HashRingEntry& o) const { return hash_point < o.hash_point; }
    };
    std::vector<HashRingEntry> hash_ring_;
    mutable std::shared_mutex ring_mutex_;
    
    static constexpr size_t VNODES_PER_SHARD = 32;

public:
    explicit DynamicShardManager(size_t initial_shards)
        : num_shards_(initial_shards)
    {
        rebuild_hash_ring();
    }

    size_t get_num_shards() const { 
        return num_shards_.load(std::memory_order_acquire); 
    }

    uint64_t get_schema_version() const {
        return schema_version_.load(std::memory_order_acquire);
    }

    // Routing con soporte para migración
    size_t route(const Key& key) const {
        // Primero verificar redirect index (lecturas frecuentes)
        {
            std::shared_lock<std::shared_mutex> lock(redirect_mutex_);
            auto it = redirect_index_.find(key);
            if (it != redirect_index_.end()) {
                return it->second;
            }
        }
        
        // Routing normal via consistent hash
        return route_via_ring(key);
    }

    // Registrar que una key fue migrada
    void record_migration(const Key& key, size_t new_shard) {
        std::unique_lock<std::shared_mutex> lock(redirect_mutex_);
        redirect_index_[key] = new_shard;
    }

    // Limpiar redirección (después de que la migración está completa)
    void clear_redirect(const Key& key) {
        std::unique_lock<std::shared_mutex> lock(redirect_mutex_);
        redirect_index_.erase(key);
    }

    // Añadir un nuevo shard (escalar horizontalmente)
    void add_shard() {
        size_t new_shard = num_shards_.fetch_add(1, std::memory_order_acq_rel);
        schema_version_.fetch_add(1, std::memory_order_release);
        
        // Añadir vnodes al ring
        std::unique_lock<std::shared_mutex> lock(ring_mutex_);
        add_shard_to_ring(new_shard);
        std::sort(hash_ring_.begin(), hash_ring_.end());
    }

    // Verificar si una key necesita migración (lazy migration)
    struct MigrationCheck {
        bool needs_migration;
        size_t current_shard;
        size_t target_shard;
    };

    MigrationCheck check_migration(const Key& key, size_t current_shard) const {
        MigrationCheck result{false, current_shard, current_shard};
        
        size_t ideal_shard = route_via_ring(key);
        if (ideal_shard != current_shard) {
            result.needs_migration = true;
            result.target_shard = ideal_shard;
        }
        
        return result;
    }

private:
    size_t hash_key(const Key& key) const {
        size_t h = std::hash<Key>{}(key);
        // Murmur3 finalizer
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ULL;
        h ^= h >> 33;
        return h;
    }

    void add_shard_to_ring(size_t shard_id) {
        for (size_t v = 0; v < VNODES_PER_SHARD; ++v) {
            size_t point = hash_key(shard_id * VNODES_PER_SHARD + v);
            hash_ring_.push_back({shard_id, point});
        }
    }

    void rebuild_hash_ring() {
        std::unique_lock<std::shared_mutex> lock(ring_mutex_);
        hash_ring_.clear();
        hash_ring_.reserve(num_shards_ * VNODES_PER_SHARD);
        
        for (size_t s = 0; s < num_shards_; ++s) {
            add_shard_to_ring(s);
        }
        std::sort(hash_ring_.begin(), hash_ring_.end());
    }

    size_t route_via_ring(const Key& key) const {
        std::shared_lock<std::shared_mutex> lock(ring_mutex_);
        
        if (hash_ring_.empty()) return 0;
        
        size_t h = hash_key(key);
        auto it = std::lower_bound(hash_ring_.begin(), hash_ring_.end(), 
            HashRingEntry{0, h});
        
        if (it == hash_ring_.end()) {
            it = hash_ring_.begin();
        }
        
        return it->shard_id;
    }
};


// -----------------------------------------------------------------------------
// Componente 3: Hooks para Extensión Distribuida
// -----------------------------------------------------------------------------
template<typename Key, typename Value>
class DistributedHooks {
public:
    // Tipo de callback para operaciones remotas
    using RemoteInsertFn = std::function<bool(size_t node_id, const Key&, const Value&)>;
    using RemoteGetFn = std::function<std::optional<Value>(size_t node_id, const Key&)>;
    using RemoteRemoveFn = std::function<bool(size_t node_id, const Key&)>;
    using NodeHealthFn = std::function<bool(size_t node_id)>;

private:
    RemoteInsertFn remote_insert_;
    RemoteGetFn remote_get_;
    RemoteRemoveFn remote_remove_;
    NodeHealthFn node_health_;
    
    size_t local_node_id_;
    size_t total_nodes_;
    std::vector<size_t> node_to_shards_;  // Mapeo: nodo -> shards que maneja
    
    bool distributed_mode_{false};

public:
    DistributedHooks() : local_node_id_(0), total_nodes_(1) {}

    void enable_distributed_mode(size_t node_id, size_t total_nodes) {
        local_node_id_ = node_id;
        total_nodes_ = total_nodes;
        distributed_mode_ = true;
    }

    void set_remote_insert(RemoteInsertFn fn) { remote_insert_ = std::move(fn); }
    void set_remote_get(RemoteGetFn fn) { remote_get_ = std::move(fn); }
    void set_remote_remove(RemoteRemoveFn fn) { remote_remove_ = std::move(fn); }
    void set_node_health_check(NodeHealthFn fn) { node_health_ = std::move(fn); }

    bool is_distributed() const { return distributed_mode_; }
    size_t get_local_node_id() const { return local_node_id_; }

    // Determinar qué nodo maneja un shard
    size_t get_node_for_shard(size_t shard_idx, size_t total_shards) const {
        if (!distributed_mode_) return local_node_id_;
        
        // Simple: distribución round-robin de shards entre nodos
        size_t shards_per_node = (total_shards + total_nodes_ - 1) / total_nodes_;
        return shard_idx / shards_per_node;
    }

    bool is_local_shard(size_t shard_idx, size_t total_shards) const {
        return get_node_for_shard(shard_idx, total_shards) == local_node_id_;
    }

    // Operaciones remotas (delegan a callbacks)
    bool remote_insert(size_t node_id, const Key& key, const Value& value) {
        if (remote_insert_) return remote_insert_(node_id, key, value);
        return false;
    }

    std::optional<Value> remote_get(size_t node_id, const Key& key) {
        if (remote_get_) return remote_get_(node_id, key);
        return std::nullopt;
    }

    bool remote_remove(size_t node_id, const Key& key) {
        if (remote_remove_) return remote_remove_(node_id, key);
        return false;
    }

    bool is_node_healthy(size_t node_id) {
        if (node_health_) return node_health_(node_id);
        return node_id == local_node_id_;  // Solo el local está "healthy" por defecto
    }
};


// -----------------------------------------------------------------------------
// AVLTreeParallelV2: Implementación Principal
// -----------------------------------------------------------------------------
template<typename Key, typename Value = Key>
class AVLTreeParallelV2 : public BaseTree<Key, Value> {
public:
    enum class RoutingStrategy {
        HASH,
        RANGE,
        CONSISTENT_HASH,
        PREDICTIVE  // Usa el HotspotPredictor
    };

private:
    // Shard con shared_mutex para lecturas concurrentes
    struct TreeShard {
        AVLTree<Key, Value> tree;
        mutable std::shared_mutex rw_lock;  // shared para reads, unique para writes
        std::atomic<size_t> local_size{0};
        std::atomic<size_t> read_count{0};
        std::atomic<size_t> write_count{0};
        
        Key min_key;
        Key max_key;
        
        TreeShard() : min_key(Key()), max_key(Key()) {}
    };

    std::vector<std::unique_ptr<TreeShard>> shards_;
    size_t num_shards_;
    RoutingStrategy routing_;

    // Componentes V2
    std::unique_ptr<HotspotPredictor<Key>> predictor_;
    std::unique_ptr<DynamicShardManager<Key, Value>> shard_manager_;
    std::unique_ptr<DistributedHooks<Key, Value>> distributed_hooks_;

    // Configuración
    bool use_prediction_{true};
    bool enable_metrics_{true};

    // Routing
    size_t getShardIndex(const Key& key) const {
        if (shard_manager_) {
            return shard_manager_->route(key);
        }
        
        switch (routing_) {
            case RoutingStrategy::HASH:
                return std::hash<Key>{}(key) % num_shards_;
            case RoutingStrategy::RANGE:
                return static_cast<size_t>(key) % num_shards_;
            case RoutingStrategy::CONSISTENT_HASH:
            case RoutingStrategy::PREDICTIVE:
                return std::hash<Key>{}(key) % num_shards_;
            default:
                return std::hash<Key>{}(key) % num_shards_;
        }
    }

    size_t getShardIndexPredictive(const Key& key) {
        size_t natural = getShardIndex(key);
        
        if (!use_prediction_ || !predictor_) {
            return natural;
        }

        // Verificar predicción de hotspot
        auto pred = predictor_->predict_hotspot(natural);
        
        if (pred.will_be_hotspot && pred.confidence > 0.7) {
            // Redirigir al shard más frío
            return predictor_->get_coolest_shard();
        }
        
        return natural;
    }

public:
    AVLTreeParallelV2(
        size_t num_shards = std::thread::hardware_concurrency(),
        RoutingStrategy routing = RoutingStrategy::HASH,
        bool enable_predictions = true)
        : num_shards_(num_shards)
        , routing_(routing)
        , use_prediction_(enable_predictions)
    {
        // Crear shards
        for (size_t i = 0; i < num_shards_; ++i) {
            shards_.push_back(std::make_unique<TreeShard>());
        }

        // Inicializar componentes V2
        predictor_ = std::make_unique<HotspotPredictor<Key>>(num_shards_);
        shard_manager_ = std::make_unique<DynamicShardManager<Key, Value>>(num_shards_);
        distributed_hooks_ = std::make_unique<DistributedHooks<Key, Value>>();
    }

    ~AVLTreeParallelV2() override = default;

    // =========================================================================
    // Operaciones básicas con shared_mutex
    // =========================================================================

    void insert(const Key& key, const Value& value = Value()) override {
        size_t shard_idx = routing_ == RoutingStrategy::PREDICTIVE 
            ? getShardIndexPredictive(key) 
            : getShardIndex(key);
        
        auto& shard = shards_[shard_idx];

        // Write lock (exclusivo)
        std::unique_lock<std::shared_mutex> lock(shard->rw_lock);

        size_t old_size = shard->tree.size();
        shard->tree.insert(key, value);
        size_t new_size = shard->tree.size();

        if (new_size > old_size) {
            shard->local_size.fetch_add(1, std::memory_order_relaxed);
        }
        shard->write_count.fetch_add(1, std::memory_order_relaxed);

        // Registrar para predicción
        if (enable_metrics_ && predictor_) {
            predictor_->record_access(shard_idx, true);
        }
    }

    void remove(const Key& key) override {
        size_t shard_idx = getShardIndex(key);
        auto& shard = shards_[shard_idx];

        // Write lock
        std::unique_lock<std::shared_mutex> lock(shard->rw_lock);

        size_t old_size = shard->tree.size();
        shard->tree.remove(key);
        size_t new_size = shard->tree.size();

        if (new_size < old_size) {
            shard->local_size.fetch_sub(1, std::memory_order_relaxed);
        }
        shard->write_count.fetch_add(1, std::memory_order_relaxed);
    }

    // LECTURA CONCURRENTE: múltiples threads pueden leer simultáneamente
    bool contains(const Key& key) const override {
        size_t shard_idx = getShardIndex(key);
        auto& shard = shards_[shard_idx];

        // Shared lock (permite lecturas concurrentes)
        std::shared_lock<std::shared_mutex> lock(shard->rw_lock);
        
        shard->read_count.fetch_add(1, std::memory_order_relaxed);
        
        if (enable_metrics_ && predictor_) {
            predictor_->record_access(shard_idx, false);
        }
        
        return shard->tree.contains(key);
    }

    Value get(const Key& key) const override {
        size_t shard_idx = getShardIndex(key);
        auto& shard = shards_[shard_idx];

        // Shared lock
        std::shared_lock<std::shared_mutex> lock(shard->rw_lock);
        
        shard->read_count.fetch_add(1, std::memory_order_relaxed);
        
        return shard->tree.get(key);
    }

    std::size_t size() const override {
        std::size_t total = 0;
        for (const auto& shard : shards_) {
            total += shard->local_size.load(std::memory_order_relaxed);
        }
        return total;
    }

    void clear() {
        for (auto& shard : shards_) {
            std::unique_lock<std::shared_mutex> lock(shard->rw_lock);
            shard->tree.clear();
            shard->local_size.store(0, std::memory_order_relaxed);
            shard->read_count.store(0, std::memory_order_relaxed);
            shard->write_count.store(0, std::memory_order_relaxed);
        }
        if (predictor_) predictor_->reset();
    }

    // =========================================================================
    // API para Trabajos Futuros
    // =========================================================================

    // Predictor de hotspots
    HotspotPredictor<Key>* get_predictor() { return predictor_.get(); }
    const HotspotPredictor<Key>* get_predictor() const { return predictor_.get(); }

    // Gestor de shards dinámicos
    DynamicShardManager<Key, Value>* get_shard_manager() { return shard_manager_.get(); }

    // Hooks distribuidos
    DistributedHooks<Key, Value>* get_distributed_hooks() { return distributed_hooks_.get(); }

    // Añadir shard dinámicamente (escalado horizontal)
    void add_shard() {
        auto new_shard = std::make_unique<TreeShard>();
        shards_.push_back(std::move(new_shard));
        num_shards_++;
        
        if (shard_manager_) {
            shard_manager_->add_shard();
        }
    }

    // Configuración
    void set_prediction_enabled(bool enabled) { use_prediction_ = enabled; }
    void set_metrics_enabled(bool enabled) { enable_metrics_ = enabled; }

    // =========================================================================
    // Estadísticas Avanzadas
    // =========================================================================

    struct ShardStatsV2 {
        size_t shard_id;
        size_t element_count;
        size_t read_count;
        size_t write_count;
        double read_write_ratio;
        double load_percentage;
        bool predicted_hotspot;
        double hotspot_confidence;
    };

    std::vector<ShardStatsV2> getShardStats() const {
        std::vector<ShardStatsV2> stats;
        size_t total = size();

        for (size_t i = 0; i < num_shards_; ++i) {
            ShardStatsV2 s;
            s.shard_id = i;
            s.element_count = shards_[i]->local_size.load(std::memory_order_relaxed);
            s.read_count = shards_[i]->read_count.load(std::memory_order_relaxed);
            s.write_count = shards_[i]->write_count.load(std::memory_order_relaxed);
            s.read_write_ratio = s.write_count > 0 
                ? static_cast<double>(s.read_count) / s.write_count 
                : s.read_count;
            s.load_percentage = total > 0 ? (100.0 * s.element_count / total) : 0.0;
            
            if (predictor_) {
                auto pred = predictor_->predict_hotspot(i);
                s.predicted_hotspot = pred.will_be_hotspot;
                s.hotspot_confidence = pred.confidence;
            } else {
                s.predicted_hotspot = false;
                s.hotspot_confidence = 0.0;
            }
            
            stats.push_back(s);
        }

        return stats;
    }

    struct ArchitectureInfoV2 {
        size_t num_shards;
        size_t hardware_concurrency;
        RoutingStrategy routing;
        size_t total_elements;
        double avg_elements_per_shard;
        double load_balance_score;
        size_t total_reads;
        size_t total_writes;
        double global_read_write_ratio;
        bool prediction_enabled;
        bool distributed_mode;
        uint64_t schema_version;
    };

    ArchitectureInfoV2 getArchitectureInfo() const {
        ArchitectureInfoV2 info;
        info.num_shards = num_shards_;
        info.hardware_concurrency = std::thread::hardware_concurrency();
        info.routing = routing_;
        info.total_elements = size();
        info.avg_elements_per_shard = static_cast<double>(info.total_elements) / num_shards_;
        
        // Calcular balance score
        auto stats = getShardStats();
        double variance = 0.0;
        info.total_reads = 0;
        info.total_writes = 0;
        
        for (const auto& s : stats) {
            double diff = s.element_count - info.avg_elements_per_shard;
            variance += diff * diff;
            info.total_reads += s.read_count;
            info.total_writes += s.write_count;
        }
        variance /= num_shards_;
        double std_dev = std::sqrt(variance);
        
        info.load_balance_score = info.avg_elements_per_shard > 0
            ? std::max(0.0, 1.0 - (std_dev / info.avg_elements_per_shard))
            : 1.0;
        
        info.global_read_write_ratio = info.total_writes > 0
            ? static_cast<double>(info.total_reads) / info.total_writes
            : info.total_reads;
        
        info.prediction_enabled = use_prediction_;
        info.distributed_mode = distributed_hooks_ && distributed_hooks_->is_distributed();
        info.schema_version = shard_manager_ ? shard_manager_->get_schema_version() : 0;
        
        return info;
    }

    void printDistribution() const {
        auto stats = getShardStats();
        auto info = getArchitectureInfo();

        std::cout << "\n╔══════════════════════════════════════════════════════╗" << std::endl;
        std::cout << "║  Parallel Tree V2 - Extended Architecture            ║" << std::endl;
        std::cout << "╚══════════════════════════════════════════════════════╝\n" << std::endl;

        std::cout << "Shards: " << info.num_shards << std::endl;
        std::cout << "Hardware threads: " << info.hardware_concurrency << std::endl;
        std::cout << "Total elements: " << info.total_elements << std::endl;
        std::cout << "Avg per shard: " << std::fixed << std::setprecision(1) 
                  << info.avg_elements_per_shard << std::endl;
        std::cout << "Balance score: " << std::setprecision(1) 
                  << (info.load_balance_score * 100) << "%" << std::endl;
        std::cout << "Read/Write ratio: " << std::setprecision(2) 
                  << info.global_read_write_ratio << std::endl;
        std::cout << "Prediction: " << (info.prediction_enabled ? "ON" : "OFF") << std::endl;
        std::cout << "Distributed: " << (info.distributed_mode ? "YES" : "NO") << std::endl;

        std::cout << "\nShard Distribution:" << std::endl;
        std::cout << "  ID   Elements   Reads   Writes   R/W    Hotspot?" << std::endl;
        std::cout << "  ──   ────────   ─────   ──────   ───    ────────" << std::endl;
        
        for (const auto& s : stats) {
            std::cout << "  " << std::setw(2) << s.shard_id 
                      << "   " << std::setw(8) << s.element_count
                      << "   " << std::setw(5) << s.read_count
                      << "   " << std::setw(6) << s.write_count
                      << "   " << std::setw(5) << std::setprecision(1) << s.read_write_ratio;
            
            if (s.predicted_hotspot) {
                std::cout << "   ⚠️  " << std::setprecision(0) 
                          << (s.hotspot_confidence * 100) << "%";
            }
            std::cout << std::endl;
        }
    }

    size_t getNumShards() const { return num_shards_; }
};

#endif // AVL_TREE_PARALLEL_V2_H
