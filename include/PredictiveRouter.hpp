#ifndef PREDICTIVE_ROUTER_HPP
#define PREDICTIVE_ROUTER_HPP

#include <vector>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <functional>
#include <random>
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <chrono>
#include <array>
#include <optional>

// =============================================================================
// Predictive Router - ML-lite Routing con Heurísticas Adaptativas
// =============================================================================
//
// Características:
// 1. EMA (Exponential Moving Average) para suavizado de carga
// 2. Detección predictiva de hotspots (antes de que ocurran)
// 3. Patrones temporales (hora del día, día de la semana)
// 4. Umbral adaptativo basado en varianza histórica
// 5. Migración lazy de keys
//
// =============================================================================

template<typename Key>
class PredictiveRouter {
public:
    enum class Strategy {
        STATIC_HASH,       // Baseline: hash simple
        LOAD_AWARE,        // Reactivo: redistribuye cuando detecta hotspot
        CONSISTENT_HASH,   // Virtual nodes para mejor distribución
        PREDICTIVE,        // ML-lite: predice hotspots antes de que ocurran
        HYBRID             // Combina predictivo con consistent hash
    };

    // -------------------------------------------------------------------------
    // Estructuras de Métricas
    // -------------------------------------------------------------------------
    
    struct ShardMetrics {
        // EMA de diferentes ventanas temporales
        double ema_short;      // Ventana corta (últimos ~10 accesos)
        double ema_medium;     // Ventana media (últimos ~100 accesos)
        double ema_long;       // Ventana larga (últimos ~1000 accesos)
        
        // Tendencias
        double trend_short;    // Derivada de ema_short
        double trend_medium;   // Derivada de ema_medium
        
        // Varianza para confidence intervals
        double variance;
        
        // Contadores
        std::atomic<size_t> total_ops{0};
        std::atomic<size_t> read_ops{0};
        std::atomic<size_t> write_ops{0};
        std::atomic<size_t> redirected_ops{0};
        
        // Timestamps
        std::chrono::steady_clock::time_point last_access;
        
        ShardMetrics() 
            : ema_short(0), ema_medium(0), ema_long(0)
            , trend_short(0), trend_medium(0), variance(0)
            , last_access(std::chrono::steady_clock::now()) {}
    };

    struct TemporalPattern {
        // Carga promedio por hora (0-23)
        std::array<double, 24> hourly_load{};
        // Carga promedio por día de semana (0-6, 0=domingo)
        std::array<double, 7> daily_load{};
        // Contador de muestras
        std::atomic<size_t> samples{0};
    };

    struct Prediction {
        bool will_be_hotspot;
        double probability;      // 0.0 - 1.0
        double predicted_load;
        double time_to_hotspot;  // Segundos estimados (-1 si no aplica)
        size_t recommended_shard;
    };

    struct Stats {
        size_t total_load;
        size_t min_load;
        size_t max_load;
        double avg_load;
        double balance_score;
        bool has_hotspot;
        size_t hotspot_shard;
        size_t predictions_made;
        size_t successful_predictions;
        double prediction_accuracy;
    };

private:
    size_t num_shards_;
    Strategy strategy_;
    
    // Métricas por shard
    std::vector<ShardMetrics> metrics_;
    std::vector<TemporalPattern> temporal_;
    mutable std::shared_mutex metrics_mutex_;
    
    // Consistent hash ring
    struct VNode {
        size_t shard_id;
        size_t hash_point;
        bool operator<(const VNode& o) const { return hash_point < o.hash_point; }
    };
    std::vector<VNode> hash_ring_;
    mutable std::shared_mutex ring_mutex_;
    
    // Redirect index para migración lazy
    std::unordered_map<Key, size_t> redirect_index_;
    mutable std::shared_mutex redirect_mutex_;
    
    // Configuración EMA
    static constexpr double ALPHA_SHORT = 0.3;   // Reactivo
    static constexpr double ALPHA_MEDIUM = 0.1;  // Balanceado
    static constexpr double ALPHA_LONG = 0.03;   // Estable
    
    // Configuración de hotspots
    double hotspot_threshold_;      // Múltiplo del promedio
    double prediction_confidence_;  // Mínimo para actuar
    
    // Estadísticas de predicción
    std::atomic<size_t> predictions_made_{0};
    std::atomic<size_t> predictions_correct_{0};
    
    // Random para desempate
    mutable std::mt19937 rng_;
    mutable std::mutex rng_mutex_;
    
    static constexpr size_t VNODES_PER_SHARD = 32;

    // -------------------------------------------------------------------------
    // Hash Functions
    // -------------------------------------------------------------------------
    
    size_t robust_hash(const Key& key) const {
        size_t h = std::hash<Key>{}(key);
        // Murmur3 finalizer
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ULL;
        h ^= h >> 33;
        return h;
    }

    size_t secondary_hash(const Key& key) const {
        size_t h = std::hash<Key>{}(key);
        h ^= (h >> 16);
        h *= 0x85ebca6b;
        h ^= (h >> 13);
        h *= 0xc2b2ae35;
        h ^= (h >> 16);
        return h;
    }

    // -------------------------------------------------------------------------
    // Inicialización
    // -------------------------------------------------------------------------
    
    void init_hash_ring() {
        std::unique_lock lock(ring_mutex_);
        hash_ring_.clear();
        hash_ring_.reserve(num_shards_ * VNODES_PER_SHARD);
        
        for (size_t s = 0; s < num_shards_; ++s) {
            for (size_t v = 0; v < VNODES_PER_SHARD; ++v) {
                size_t point = secondary_hash(s * VNODES_PER_SHARD + v);
                hash_ring_.push_back({s, point});
            }
        }
        std::sort(hash_ring_.begin(), hash_ring_.end());
    }

    // -------------------------------------------------------------------------
    // Actualización de Métricas
    // -------------------------------------------------------------------------
    
    void update_metrics(size_t shard_idx, bool is_write) {
        auto now = std::chrono::steady_clock::now();
        
        std::unique_lock lock(metrics_mutex_);
        auto& m = metrics_[shard_idx];
        
        // Calcular tasa instantánea
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m.last_access
        ).count();
        
        double instant_rate = elapsed_ms > 0 ? 1000.0 / elapsed_ms : 100.0;
        
        // Guardar valores anteriores para tendencia
        double old_short = m.ema_short;
        double old_medium = m.ema_medium;
        
        // Actualizar EMAs
        m.ema_short = ALPHA_SHORT * instant_rate + (1 - ALPHA_SHORT) * m.ema_short;
        m.ema_medium = ALPHA_MEDIUM * instant_rate + (1 - ALPHA_MEDIUM) * m.ema_medium;
        m.ema_long = ALPHA_LONG * instant_rate + (1 - ALPHA_LONG) * m.ema_long;
        
        // Actualizar tendencias
        m.trend_short = m.ema_short - old_short;
        m.trend_medium = m.ema_medium - old_medium;
        
        // Actualizar varianza (EMA del error cuadrado)
        double error = instant_rate - m.ema_medium;
        m.variance = ALPHA_MEDIUM * (error * error) + (1 - ALPHA_MEDIUM) * m.variance;
        
        // Contadores
        m.total_ops.fetch_add(1, std::memory_order_relaxed);
        if (is_write) {
            m.write_ops.fetch_add(1, std::memory_order_relaxed);
        } else {
            m.read_ops.fetch_add(1, std::memory_order_relaxed);
        }
        
        m.last_access = now;
        
        // Actualizar patrón temporal
        auto time_t = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now()
        );
        std::tm* tm = std::localtime(&time_t);
        
        auto& tp = temporal_[shard_idx];
        int hour = tm->tm_hour;
        int day = tm->tm_wday;
        
        tp.hourly_load[hour] = ALPHA_MEDIUM * instant_rate + 
                               (1 - ALPHA_MEDIUM) * tp.hourly_load[hour];
        tp.daily_load[day] = ALPHA_LONG * instant_rate + 
                             (1 - ALPHA_LONG) * tp.daily_load[day];
        tp.samples.fetch_add(1, std::memory_order_relaxed);
    }

    // -------------------------------------------------------------------------
    // Predicción de Hotspots
    // -------------------------------------------------------------------------
    
    Prediction predict_hotspot_internal(size_t shard_idx) const {
        Prediction pred{false, 0.0, 0.0, -1.0, shard_idx};
        
        std::shared_lock lock(metrics_mutex_);
        
        // Calcular estadísticas globales
        double total_load = 0;
        double max_load = 0;
        size_t max_shard = 0;
        
        for (size_t i = 0; i < num_shards_; ++i) {
            total_load += metrics_[i].ema_medium;
            if (metrics_[i].ema_medium > max_load) {
                max_load = metrics_[i].ema_medium;
                max_shard = i;
            }
        }
        double avg_load = total_load / num_shards_;
        
        // Calcular desviación estándar
        double variance = 0;
        for (size_t i = 0; i < num_shards_; ++i) {
            double diff = metrics_[i].ema_medium - avg_load;
            variance += diff * diff;
        }
        double std_dev = std::sqrt(variance / num_shards_);
        
        const auto& m = metrics_[shard_idx];
        
        // Umbral adaptativo: basado en std_dev pero con mínimo
        double threshold = std::max(
            avg_load * hotspot_threshold_,
            avg_load + 2.0 * std_dev
        );
        
        // Proyección a 5 segundos usando tendencia
        pred.predicted_load = m.ema_short + m.trend_short * 50.0;  // ~5 segundos
        
        // ¿Ya es hotspot?
        if (m.ema_medium > threshold) {
            pred.will_be_hotspot = true;
            pred.probability = std::min(1.0, (m.ema_medium - threshold) / (std_dev + 0.1));
            pred.time_to_hotspot = 0;
            pred.recommended_shard = find_coolest_shard_internal();
            return pred;
        }
        
        // ¿Se convertirá en hotspot? (tendencia positiva)
        if (m.trend_short > 0 && pred.predicted_load > threshold) {
            pred.will_be_hotspot = true;
            
            // Tiempo estimado hasta hotspot
            double gap = threshold - m.ema_medium;
            pred.time_to_hotspot = gap / m.trend_short * 0.1;  // Convertir a segundos aprox
            
            // Probabilidad basada en tendencia vs volatilidad
            double volatility = std::sqrt(m.variance);
            pred.probability = std::max(0.0, std::min(1.0,
                m.trend_short / (volatility + 0.1) * 0.3
            ));
            
            pred.recommended_shard = find_coolest_shard_internal();
        }
        
        // Considerar patrón temporal (hora del día)
        auto time_t = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now()
        );
        std::tm* tm = std::localtime(&time_t);
        int next_hour = (tm->tm_hour + 1) % 24;
        
        const auto& tp = temporal_[shard_idx];
        if (tp.samples.load(std::memory_order_relaxed) > 100) {
            // Si la siguiente hora históricamente tiene más carga
            if (tp.hourly_load[next_hour] > threshold) {
                pred.will_be_hotspot = true;
                pred.probability = std::max(pred.probability, 0.5);
                pred.time_to_hotspot = (60 - tm->tm_min) * 60.0;  // Segundos hasta próxima hora
            }
        }
        
        return pred;
    }

    size_t find_coolest_shard_internal() const {
        // Ya tiene lock de metrics_mutex_
        size_t best = 0;
        double best_score = std::numeric_limits<double>::max();
        
        for (size_t i = 0; i < num_shards_; ++i) {
            // Score = carga actual + tendencia ponderada
            double score = metrics_[i].ema_medium + metrics_[i].trend_medium * 10.0;
            if (score < best_score) {
                best_score = score;
                best = i;
            }
        }
        
        return best;
    }

    // -------------------------------------------------------------------------
    // Estrategias de Routing
    // -------------------------------------------------------------------------
    
    size_t route_static(const Key& key) const {
        return robust_hash(key) % num_shards_;
    }

    size_t route_consistent(const Key& key) const {
        std::shared_lock lock(ring_mutex_);
        
        if (hash_ring_.empty()) return route_static(key);
        
        size_t h = robust_hash(key);
        auto it = std::lower_bound(hash_ring_.begin(), hash_ring_.end(), VNode{0, h});
        
        if (it == hash_ring_.end()) {
            it = hash_ring_.begin();
        }
        
        return it->shard_id;
    }

    size_t route_load_aware(const Key& key, size_t natural_shard) {
        std::shared_lock lock(metrics_mutex_);
        
        const auto& m = metrics_[natural_shard];
        
        // Calcular promedio
        double total = 0;
        for (size_t i = 0; i < num_shards_; ++i) {
            total += metrics_[i].ema_medium;
        }
        double avg = total / num_shards_;
        
        // Si está sobrecargado, redistribuir
        if (m.ema_medium > avg * hotspot_threshold_) {
            return find_coolest_shard_internal();
        }
        
        return natural_shard;
    }

    size_t route_predictive(const Key& key, size_t natural_shard) {
        auto pred = predict_hotspot_internal(natural_shard);
        
        if (pred.will_be_hotspot && pred.probability >= prediction_confidence_) {
            predictions_made_.fetch_add(1, std::memory_order_relaxed);
            
            // Registrar redirección
            metrics_[natural_shard].redirected_ops.fetch_add(1, std::memory_order_relaxed);
            
            return pred.recommended_shard;
        }
        
        return natural_shard;
    }

    size_t route_hybrid(const Key& key, size_t natural_shard) {
        // Primero verificar predicción
        auto pred = predict_hotspot_internal(natural_shard);
        
        if (pred.will_be_hotspot && pred.probability >= prediction_confidence_) {
            return pred.recommended_shard;
        }
        
        // Si no hay predicción de hotspot, usar consistent hash
        return route_consistent(key);
    }

public:
    // -------------------------------------------------------------------------
    // Constructor
    // -------------------------------------------------------------------------
    
    explicit PredictiveRouter(
        size_t num_shards, 
        Strategy strategy = Strategy::PREDICTIVE,
        double hotspot_threshold = 1.5,
        double prediction_confidence = 0.6)
        : num_shards_(num_shards)
        , strategy_(strategy)
        , metrics_(num_shards)
        , temporal_(num_shards)
        , hotspot_threshold_(hotspot_threshold)
        , prediction_confidence_(prediction_confidence)
        , rng_(std::random_device{}())
    {
        if (strategy_ == Strategy::CONSISTENT_HASH || 
            strategy_ == Strategy::HYBRID) {
            init_hash_ring();
        }
    }

    // -------------------------------------------------------------------------
    // API Principal
    // -------------------------------------------------------------------------
    
    size_t route(const Key& key) {
        // Primero verificar redirect index
        {
            std::shared_lock lock(redirect_mutex_);
            auto it = redirect_index_.find(key);
            if (it != redirect_index_.end()) {
                return it->second;
            }
        }
        
        size_t natural = route_static(key);
        
        switch (strategy_) {
            case Strategy::STATIC_HASH:
                return natural;
                
            case Strategy::LOAD_AWARE:
                return route_load_aware(key, natural);
                
            case Strategy::CONSISTENT_HASH:
                return route_consistent(key);
                
            case Strategy::PREDICTIVE:
                return route_predictive(key, natural);
                
            case Strategy::HYBRID:
                return route_hybrid(key, natural);
                
            default:
                return natural;
        }
    }

    void record_access(size_t shard_idx, bool is_write = false) {
        if (shard_idx < num_shards_) {
            update_metrics(shard_idx, is_write);
        }
    }

    // Registrar que una predicción fue correcta (para tracking de accuracy)
    void record_prediction_outcome(bool was_correct) {
        if (was_correct) {
            predictions_correct_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Migración lazy: registrar que una key fue movida
    void register_migration(const Key& key, size_t new_shard) {
        std::unique_lock lock(redirect_mutex_);
        redirect_index_[key] = new_shard;
    }

    void clear_migration(const Key& key) {
        std::unique_lock lock(redirect_mutex_);
        redirect_index_.erase(key);
    }

    // -------------------------------------------------------------------------
    // API de Predicción
    // -------------------------------------------------------------------------
    
    Prediction predict(size_t shard_idx) const {
        return predict_hotspot_internal(shard_idx);
    }

    std::vector<Prediction> predict_all() const {
        std::vector<Prediction> preds;
        preds.reserve(num_shards_);
        
        for (size_t i = 0; i < num_shards_; ++i) {
            preds.push_back(predict_hotspot_internal(i));
        }
        
        return preds;
    }

    size_t get_coolest_shard() const {
        std::shared_lock lock(metrics_mutex_);
        return find_coolest_shard_internal();
    }

    // -------------------------------------------------------------------------
    // Estadísticas
    // -------------------------------------------------------------------------
    
    Stats get_stats() const {
        Stats stats{};
        stats.total_load = 0;
        stats.min_load = SIZE_MAX;
        stats.max_load = 0;
        stats.hotspot_shard = 0;
        
        std::shared_lock lock(metrics_mutex_);
        
        for (size_t i = 0; i < num_shards_; ++i) {
            size_t load = metrics_[i].total_ops.load(std::memory_order_relaxed);
            stats.total_load += load;
            if (load < stats.min_load) stats.min_load = load;
            if (load > stats.max_load) {
                stats.max_load = load;
                stats.hotspot_shard = i;
            }
        }
        
        stats.avg_load = stats.total_load / static_cast<double>(num_shards_);
        
        // Balance score
        if (stats.avg_load > 0) {
            double variance = 0;
            for (size_t i = 0; i < num_shards_; ++i) {
                double diff = metrics_[i].total_ops.load(std::memory_order_relaxed) - stats.avg_load;
                variance += diff * diff;
            }
            double std_dev = std::sqrt(variance / num_shards_);
            stats.balance_score = std::max(0.0, 1.0 - (std_dev / stats.avg_load));
        } else {
            stats.balance_score = 1.0;
        }
        
        stats.has_hotspot = (stats.max_load > hotspot_threshold_ * stats.avg_load);
        
        stats.predictions_made = predictions_made_.load(std::memory_order_relaxed);
        stats.successful_predictions = predictions_correct_.load(std::memory_order_relaxed);
        stats.prediction_accuracy = stats.predictions_made > 0 
            ? static_cast<double>(stats.successful_predictions) / stats.predictions_made 
            : 1.0;
        
        return stats;
    }

    // Obtener métricas de un shard específico
    ShardMetrics get_shard_metrics(size_t shard_idx) const {
        if (shard_idx >= num_shards_) return ShardMetrics{};
        
        std::shared_lock lock(metrics_mutex_);
        return metrics_[shard_idx];
    }

    // Obtener carga predicha para una hora específica
    double get_predicted_load(size_t shard_idx, int hour) const {
        if (shard_idx >= num_shards_ || hour < 0 || hour >= 24) return 0;
        
        std::shared_lock lock(metrics_mutex_);
        return temporal_[shard_idx].hourly_load[hour];
    }

    // -------------------------------------------------------------------------
    // Configuración
    // -------------------------------------------------------------------------
    
    void set_strategy(Strategy s) { 
        strategy_ = s; 
        if (s == Strategy::CONSISTENT_HASH || s == Strategy::HYBRID) {
            init_hash_ring();
        }
    }
    
    Strategy get_strategy() const { return strategy_; }
    
    void set_hotspot_threshold(double t) { hotspot_threshold_ = t; }
    double get_hotspot_threshold() const { return hotspot_threshold_; }
    
    void set_prediction_confidence(double c) { prediction_confidence_ = c; }
    double get_prediction_confidence() const { return prediction_confidence_; }

    size_t get_num_shards() const { return num_shards_; }

    // Reset para testing
    void reset() {
        std::unique_lock lock1(metrics_mutex_);
        std::unique_lock lock2(redirect_mutex_);
        
        metrics_.assign(num_shards_, ShardMetrics{});
        temporal_.assign(num_shards_, TemporalPattern{});
        redirect_index_.clear();
        
        predictions_made_.store(0, std::memory_order_relaxed);
        predictions_correct_.store(0, std::memory_order_relaxed);
    }
};

#endif // PREDICTIVE_ROUTER_HPP
