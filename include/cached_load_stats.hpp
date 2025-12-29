#ifndef CACHED_LOAD_STATS_HPP
#define CACHED_LOAD_STATS_HPP

#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>

// CachedLoadStats: Fix para hacer el routing realmente O(1)
//
// Problema: El load-aware routing del paper itera sobre todos los shards
// para encontrar el de menor carga → O(N), no O(1) como claimea.
//
// Solución: Background thread actualiza cached min/max cada 1ms.
// route() ahora es O(1) real consultando el caché.

class CachedLoadStats {
private:
    // Cargas por shard (actualizadas por los shards)
    std::vector<std::atomic<size_t>> loads_;

    // Cached statistics (actualizadas por background thread)
    std::atomic<size_t> min_shard_{0};
    std::atomic<size_t> max_shard_{0};
    std::atomic<size_t> min_load_{0};
    std::atomic<size_t> max_load_{0};
    std::atomic<size_t> total_load_{0};

    // Control del background thread
    std::atomic<bool> running_{false};
    std::thread refresh_thread_;

    // Configuración
    std::chrono::milliseconds refresh_interval_{1};  // 1ms por defecto

    // Background refresh loop
    void refresh_loop() {
        while (running_.load(std::memory_order_acquire)) {
            refresh();
            std::this_thread::sleep_for(refresh_interval_);
        }
    }

    // Refresh statistics (scan all shards)
    void refresh() {
        if (loads_.empty()) return;

        size_t min_load = UINT64_MAX;
        size_t max_load = 0;
        size_t total = 0;
        size_t min_idx = 0;
        size_t max_idx = 0;

        for (size_t i = 0; i < loads_.size(); ++i) {
            size_t load = loads_[i].load(std::memory_order_relaxed);
            total += load;

            if (load < min_load) {
                min_load = load;
                min_idx = i;
            }

            if (load > max_load) {
                max_load = load;
                max_idx = i;
            }
        }

        // Actualizar cached stats atómicamente
        min_shard_.store(min_idx, std::memory_order_release);
        max_shard_.store(max_idx, std::memory_order_release);
        min_load_.store(min_load, std::memory_order_release);
        max_load_.store(max_load, std::memory_order_release);
        total_load_.store(total, std::memory_order_release);
    }

public:
    explicit CachedLoadStats(size_t num_shards,
                            std::chrono::milliseconds refresh_interval = std::chrono::milliseconds(1))
        : refresh_interval_(refresh_interval)
    {
        loads_.resize(num_shards);
        for (auto& load : loads_) {
            load.store(0, std::memory_order_relaxed);
        }
    }

    ~CachedLoadStats() {
        stop();
    }

    // No copiable, no movible (tiene thread)
    CachedLoadStats(const CachedLoadStats&) = delete;
    CachedLoadStats& operator=(const CachedLoadStats&) = delete;

    // Start background refresh thread
    void start() {
        if (running_.load(std::memory_order_acquire)) {
            return;  // Ya corriendo
        }

        running_.store(true, std::memory_order_release);
        refresh_thread_ = std::thread(&CachedLoadStats::refresh_loop, this);
    }

    // Stop background thread
    void stop() {
        if (!running_.load(std::memory_order_acquire)) {
            return;  // Ya detenido
        }

        running_.store(false, std::memory_order_release);
        if (refresh_thread_.joinable()) {
            refresh_thread_.join();
        }
    }

    // Update load for a shard (llamado por ParallelAVL al insertar/remove)
    void increment_load(size_t shard_id) {
        if (shard_id < loads_.size()) {
            loads_[shard_id].fetch_add(1, std::memory_order_relaxed);
        }
    }

    void decrement_load(size_t shard_id) {
        if (shard_id < loads_.size()) {
            loads_[shard_id].fetch_sub(1, std::memory_order_relaxed);
        }
    }

    void set_load(size_t shard_id, size_t load) {
        if (shard_id < loads_.size()) {
            loads_[shard_id].store(load, std::memory_order_relaxed);
        }
    }

    // O(1) queries (read from cache)
    size_t get_min_shard() const {
        return min_shard_.load(std::memory_order_acquire);
    }

    size_t get_max_shard() const {
        return max_shard_.load(std::memory_order_acquire);
    }

    size_t get_min_load() const {
        return min_load_.load(std::memory_order_acquire);
    }

    size_t get_max_load() const {
        return max_load_.load(std::memory_order_acquire);
    }

    size_t get_total_load() const {
        return total_load_.load(std::memory_order_acquire);
    }

    double get_average_load() const {
        if (loads_.empty()) return 0.0;
        return total_load_.load(std::memory_order_acquire) / static_cast<double>(loads_.size());
    }

    size_t get_load(size_t shard_id) const {
        if (shard_id < loads_.size()) {
            return loads_[shard_id].load(std::memory_order_acquire);
        }
        return 0;
    }

    // Statistics for routing decisions
    double get_coefficient_of_variation() const {
        double mean = get_average_load();
        if (mean == 0) return 0.0;

        // Compute variance
        double variance = 0.0;
        for (const auto& load : loads_) {
            double diff = load.load(std::memory_order_relaxed) - mean;
            variance += diff * diff;
        }
        variance /= loads_.size();

        double stddev = std::sqrt(variance);
        return stddev / mean;  // CV = σ/μ
    }

    double get_balance_score() const {
        size_t min = get_min_load();
        size_t max = get_max_load();

        if (max == 0) return 1.0;
        return static_cast<double>(min) / static_cast<double>(max);
    }

    bool detect_hotspot(double threshold = 1.5) const {
        double avg = get_average_load();
        if (avg == 0) return false;

        size_t max = get_max_load();
        return (max > avg * threshold);
    }

    size_t num_shards() const {
        return loads_.size();
    }

    // Para debugging
    std::vector<size_t> snapshot() const {
        std::vector<size_t> result;
        result.reserve(loads_.size());
        for (const auto& load : loads_) {
            result.push_back(load.load(std::memory_order_acquire));
        }
        return result;
    }
};

#endif // CACHED_LOAD_STATS_HPP
