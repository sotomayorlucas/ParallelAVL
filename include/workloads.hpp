#ifndef WORKLOADS_HPP
#define WORKLOADS_HPP

#include <random>
#include <vector>
#include <cmath>
#include <algorithm>
#include <memory>

// Workload Generators para benchmarking riguroso
//
// 1. Uniform: keys uniformemente distribuidas (baseline)
// 2. Zipfian: 80/20 rule, simula acceso real (α=0.99)
// 3. Sequential: keys secuenciales (worst case para hash)
// 4. Adversarial: todos los keys van al mismo shard (stress test)

enum class WorkloadType {
    UNIFORM,
    ZIPFIAN,
    SEQUENTIAL,
    ADVERSARIAL
};

// Base class
template<typename Key = int>
class WorkloadGenerator {
public:
    virtual ~WorkloadGenerator() = default;
    virtual Key next() = 0;
    virtual void reset() = 0;
};

// Uniform distribution
template<typename Key = int>
class UniformGenerator : public WorkloadGenerator<Key> {
private:
    std::mt19937 gen_;
    std::uniform_int_distribution<Key> dist_;

public:
    UniformGenerator(Key min, Key max, unsigned seed = std::random_device{}())
        : gen_(seed), dist_(min, max)
    {}

    Key next() override {
        return dist_(gen_);
    }

    void reset() override {
        // No state to reset para uniform
    }
};

// Zipfian distribution (Pareto-like, 80/20 rule)
//
// Implementación basada en "Quickly Generating Billion-Record Synthetic Databases"
// Gray et al., SIGMOD 1994
//
// α = 0.99 → ~80% de accesos a ~20% de keys (realistic workload)
// α = 1.5  → más skewed
template<typename Key = int>
class ZipfianGenerator : public WorkloadGenerator<Key> {
private:
    size_t n_;        // Número de items
    double alpha_;    // Skew parameter
    double theta_;    // = alpha_ - 1
    double zeta_n_;   // Normalization constant
    double eta_;      // Helper for sampling

    std::mt19937 gen_;
    std::uniform_real_distribution<double> uniform_;

    // Compute zeta(n, θ) = Σ(i=1 to n) 1/i^θ
    double zeta(size_t n, double theta) {
        double sum = 0.0;
        for (size_t i = 1; i <= n; ++i) {
            sum += 1.0 / std::pow(i, theta);
        }
        return sum;
    }

public:
    ZipfianGenerator(size_t n, double alpha = 0.99, unsigned seed = std::random_device{}())
        : n_(n), alpha_(alpha), theta_(alpha - 1.0), gen_(seed), uniform_(0.0, 1.0)
    {
        zeta_n_ = zeta(n_, theta_);
        eta_ = (1.0 - std::pow(2.0 / n_, 1.0 - alpha_)) / (1.0 - zeta(2, theta_) / zeta_n_);
    }

    Key next() override {
        double u = uniform_(gen_);
        double uz = u * zeta_n_;

        if (uz < 1.0) {
            return static_cast<Key>(1);
        }

        if (uz < 1.0 + std::pow(0.5, theta_)) {
            return static_cast<Key>(2);
        }

        return static_cast<Key>(1 + n_ * std::pow(eta_ * u - eta_ + 1.0, alpha_));
    }

    void reset() override {
        // No state to reset
    }
};

// Sequential generator
template<typename Key = int>
class SequentialGenerator : public WorkloadGenerator<Key> {
private:
    Key current_;
    Key start_;

public:
    explicit SequentialGenerator(Key start = 0)
        : current_(start), start_(start)
    {}

    Key next() override {
        return current_++;
    }

    void reset() override {
        current_ = start_;
    }
};

// Adversarial generator: all keys hash to same shard
//
// Para N shards, genera keys que hacen hash(key) % N == target_shard
// Esto satura un shard específico (worst case para load balancing)
template<typename Key = int>
class AdversarialGenerator : public WorkloadGenerator<Key> {
private:
    size_t num_shards_;
    size_t target_shard_;
    Key counter_;

public:
    AdversarialGenerator(size_t num_shards, size_t target_shard = 0)
        : num_shards_(num_shards), target_shard_(target_shard), counter_(target_shard)
    {}

    Key next() override {
        Key result = counter_;
        counter_ += static_cast<Key>(num_shards_);
        return result;
    }

    void reset() override {
        counter_ = static_cast<Key>(target_shard_);
    }
};

// Hotspot generator: Mix of uniform (90%) + targeted (10%)
//
// Simula workload con hotspot: mayoría uniforme, pero 10% de accesos
// van a un pequeño conjunto de keys "populares"
template<typename Key = int>
class HotspotGenerator : public WorkloadGenerator<Key> {
private:
    std::mt19937 gen_;
    std::uniform_real_distribution<double> uniform_dist_;
    std::uniform_int_distribution<Key> cold_dist_;
    std::uniform_int_distribution<Key> hot_dist_;

    double hot_fraction_;  // % de accesos que van al hotspot

public:
    HotspotGenerator(Key cold_min, Key cold_max,
                     Key hot_min, Key hot_max,
                     double hot_fraction = 0.1,
                     unsigned seed = std::random_device{}())
        : gen_(seed),
          uniform_dist_(0.0, 1.0),
          cold_dist_(cold_min, cold_max),
          hot_dist_(hot_min, hot_max),
          hot_fraction_(hot_fraction)
    {}

    Key next() override {
        double r = uniform_dist_(gen_);

        if (r < hot_fraction_) {
            return hot_dist_(gen_);  // Hotspot access
        } else {
            return cold_dist_(gen_);  // Normal access
        }
    }

    void reset() override {
        // No state to reset
    }
};

// Factory para crear workloads
template<typename Key = int>
class WorkloadFactory {
public:
    static std::unique_ptr<WorkloadGenerator<Key>> create(
        WorkloadType type,
        size_t key_space,
        size_t num_shards = 8,
        unsigned seed = 0)
    {
        switch (type) {
            case WorkloadType::UNIFORM:
                return std::make_unique<UniformGenerator<Key>>(0, key_space - 1, seed);

            case WorkloadType::ZIPFIAN:
                return std::make_unique<ZipfianGenerator<Key>>(key_space, 0.99, seed);

            case WorkloadType::SEQUENTIAL:
                return std::make_unique<SequentialGenerator<Key>>(0);

            case WorkloadType::ADVERSARIAL:
                return std::make_unique<AdversarialGenerator<Key>>(num_shards, 0);

            default:
                return std::make_unique<UniformGenerator<Key>>(0, key_space - 1, seed);
        }
    }

    static const char* name(WorkloadType type) {
        switch (type) {
            case WorkloadType::UNIFORM:      return "UNIFORM";
            case WorkloadType::ZIPFIAN:      return "ZIPFIAN";
            case WorkloadType::SEQUENTIAL:   return "SEQUENTIAL";
            case WorkloadType::ADVERSARIAL:  return "ADVERSARIAL";
            default:                         return "UNKNOWN";
        }
    }
};

#endif // WORKLOADS_HPP
