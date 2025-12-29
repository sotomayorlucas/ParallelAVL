#ifndef DISTRIBUTED_AVL_HPP
#define DISTRIBUTED_AVL_HPP

#include "AVLTreeParallelV2.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <optional>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <chrono>
#include <memory>
#include <queue>
#include <condition_variable>
#include <future>

// =============================================================================
// Distributed AVL Tree - Multi-Node Extension
// =============================================================================
//
// Arquitectura:
//
//          [Client Layer]
//                |
//         [Router/Coordinator]
//                |
//    +------+----+----+------+
//    |      |         |      |
// [Node 0] [Node 1] [Node 2] [Node 3]
// Shards   Shards   Shards   Shards
// 0-7      8-15     16-23    24-31
//
// Cada nodo corre un AVLTreeParallelV2 con un subconjunto de shards.
// El coordinador decide qué nodo maneja cada key.
//
// Opciones de consistencia:
// - STRONG: Writes sincrónicos, lecturas del primary
// - EVENTUAL: Writes async, lecturas del local (puede ser stale)
// - CAUSAL: Preserva orden de operaciones relacionadas
//
// =============================================================================

namespace distributed {

// -----------------------------------------------------------------------------
// Tipos y Configuración
// -----------------------------------------------------------------------------

using NodeId = uint32_t;
using ShardId = uint32_t;
using Version = uint64_t;

enum class ConsistencyLevel {
    STRONG,     // Linearizable
    EVENTUAL,   // Fast but potentially stale
    CAUSAL      // Preserves causality
};

enum class ReplicationMode {
    NONE,           // No replication
    SYNC,           // Synchronous replication
    ASYNC,          // Asynchronous replication
    SEMI_SYNC       // Wait for at least one replica
};

struct ClusterConfig {
    size_t num_nodes = 1;
    size_t shards_per_node = 8;
    size_t replication_factor = 1;  // 1 = no replication
    ConsistencyLevel consistency = ConsistencyLevel::STRONG;
    ReplicationMode replication = ReplicationMode::NONE;
    std::chrono::milliseconds write_timeout{1000};
    std::chrono::milliseconds read_timeout{500};
};

// -----------------------------------------------------------------------------
// Node Health & Status
// -----------------------------------------------------------------------------

enum class NodeStatus {
    HEALTHY,
    DEGRADED,
    UNHEALTHY,
    OFFLINE
};

struct NodeHealth {
    NodeId id;
    NodeStatus status;
    std::chrono::steady_clock::time_point last_heartbeat;
    double load_factor;      // 0.0 - 1.0
    size_t pending_ops;
    size_t failed_ops;
    std::string address;
    uint16_t port;
};

// -----------------------------------------------------------------------------
// Message Types (para comunicación entre nodos)
// -----------------------------------------------------------------------------

enum class MessageType {
    // Operaciones CRUD
    INSERT,
    GET,
    REMOVE,
    CONTAINS,
    
    // Replicación
    REPLICATE,
    ACK,
    NACK,
    
    // Cluster management
    HEARTBEAT,
    JOIN,
    LEAVE,
    GOSSIP,
    
    // Migración
    MIGRATE_START,
    MIGRATE_DATA,
    MIGRATE_COMPLETE
};

template<typename Key, typename Value>
struct Message {
    MessageType type;
    NodeId source;
    NodeId target;
    Version version;
    Key key;
    std::optional<Value> value;
    std::chrono::steady_clock::time_point timestamp;
    uint64_t request_id;
    
    Message() 
        : type(MessageType::HEARTBEAT)
        , source(0), target(0), version(0)
        , timestamp(std::chrono::steady_clock::now())
        , request_id(0) {}
};

// -----------------------------------------------------------------------------
// Transport Interface (abstracta - implementar según protocolo)
// -----------------------------------------------------------------------------

template<typename Key, typename Value>
class ITransport {
public:
    virtual ~ITransport() = default;
    
    // Enviar mensaje a un nodo específico
    virtual bool send(NodeId target, const Message<Key, Value>& msg) = 0;
    
    // Enviar a todos los nodos
    virtual void broadcast(const Message<Key, Value>& msg) = 0;
    
    // Callback para mensajes recibidos
    using MessageHandler = std::function<void(const Message<Key, Value>&)>;
    virtual void set_message_handler(MessageHandler handler) = 0;
    
    // Gestión de conexiones
    virtual bool connect(NodeId id, const std::string& address, uint16_t port) = 0;
    virtual void disconnect(NodeId id) = 0;
    virtual bool is_connected(NodeId id) const = 0;
};

// -----------------------------------------------------------------------------
// Stub Transport (para testing local)
// -----------------------------------------------------------------------------

template<typename Key, typename Value>
class LocalTransport : public ITransport<Key, Value> {
private:
    using Handler = typename ITransport<Key, Value>::MessageHandler;
    
    std::unordered_map<NodeId, Handler> handlers_;
    std::unordered_map<NodeId, bool> connected_;
    mutable std::mutex mutex_;

public:
    bool send(NodeId target, const Message<Key, Value>& msg) override {
        std::lock_guard lock(mutex_);
        auto it = handlers_.find(target);
        if (it != handlers_.end() && connected_[target]) {
            it->second(msg);
            return true;
        }
        return false;
    }
    
    void broadcast(const Message<Key, Value>& msg) override {
        std::lock_guard lock(mutex_);
        for (const auto& [id, handler] : handlers_) {
            if (connected_[id]) {
                handler(msg);
            }
        }
    }
    
    void set_message_handler(Handler handler) override {
        // En LocalTransport, cada nodo registra su handler por separado
    }
    
    void register_node(NodeId id, Handler handler) {
        std::lock_guard lock(mutex_);
        handlers_[id] = std::move(handler);
        connected_[id] = true;
    }
    
    bool connect(NodeId id, const std::string&, uint16_t) override {
        std::lock_guard lock(mutex_);
        connected_[id] = true;
        return true;
    }
    
    void disconnect(NodeId id) override {
        std::lock_guard lock(mutex_);
        connected_[id] = false;
    }
    
    bool is_connected(NodeId id) const override {
        std::lock_guard lock(mutex_);
        auto it = connected_.find(id);
        return it != connected_.end() && it->second;
    }
};

// -----------------------------------------------------------------------------
// Distributed Coordinator
// -----------------------------------------------------------------------------

template<typename Key, typename Value>
class DistributedCoordinator {
private:
    ClusterConfig config_;
    NodeId local_node_id_;
    
    // Estado del cluster
    std::vector<NodeHealth> nodes_;
    mutable std::shared_mutex nodes_mutex_;
    
    // Mapeo shard -> nodos (primary + replicas)
    struct ShardAssignment {
        NodeId primary;
        std::vector<NodeId> replicas;
    };
    std::vector<ShardAssignment> shard_map_;
    mutable std::shared_mutex shard_mutex_;
    
    // Version vectors para consistencia causal
    std::unordered_map<NodeId, Version> version_vector_;
    std::atomic<Version> local_version_{0};
    
    // Transport
    std::shared_ptr<ITransport<Key, Value>> transport_;
    
    // Pending requests (para operaciones async)
    struct PendingRequest {
        uint64_t id;
        std::promise<std::optional<Value>> promise;
        std::chrono::steady_clock::time_point deadline;
    };
    std::unordered_map<uint64_t, PendingRequest> pending_;
    std::mutex pending_mutex_;
    std::atomic<uint64_t> next_request_id_{0};

public:
    explicit DistributedCoordinator(
        const ClusterConfig& config,
        NodeId local_id,
        std::shared_ptr<ITransport<Key, Value>> transport = nullptr)
        : config_(config)
        , local_node_id_(local_id)
        , transport_(transport)
    {
        // Inicializar shard map con distribución round-robin
        size_t total_shards = config_.num_nodes * config_.shards_per_node;
        shard_map_.resize(total_shards);
        
        for (size_t s = 0; s < total_shards; ++s) {
            NodeId primary = static_cast<NodeId>(s / config_.shards_per_node);
            shard_map_[s].primary = primary;
            
            // Asignar réplicas (si hay replicación)
            if (config_.replication_factor > 1) {
                for (size_t r = 1; r < config_.replication_factor; ++r) {
                    NodeId replica = static_cast<NodeId>((primary + r) % config_.num_nodes);
                    shard_map_[s].replicas.push_back(replica);
                }
            }
        }
        
        // Inicializar estado de nodos
        nodes_.resize(config_.num_nodes);
        for (size_t i = 0; i < config_.num_nodes; ++i) {
            nodes_[i].id = static_cast<NodeId>(i);
            nodes_[i].status = NodeStatus::HEALTHY;
            nodes_[i].last_heartbeat = std::chrono::steady_clock::now();
            nodes_[i].load_factor = 0.0;
            nodes_[i].pending_ops = 0;
            nodes_[i].failed_ops = 0;
        }
    }

    // Determinar qué nodo maneja una key
    NodeId get_primary_node(const Key& key) const {
        size_t shard = get_shard(key);
        std::shared_lock lock(shard_mutex_);
        return shard_map_[shard].primary;
    }

    std::vector<NodeId> get_replica_nodes(const Key& key) const {
        size_t shard = get_shard(key);
        std::shared_lock lock(shard_mutex_);
        return shard_map_[shard].replicas;
    }

    bool is_local(const Key& key) const {
        return get_primary_node(key) == local_node_id_;
    }

    // Obtener estado del cluster
    std::vector<NodeHealth> get_cluster_state() const {
        std::shared_lock lock(nodes_mutex_);
        return nodes_;
    }

    NodeHealth get_node_health(NodeId id) const {
        std::shared_lock lock(nodes_mutex_);
        if (id < nodes_.size()) {
            return nodes_[id];
        }
        return NodeHealth{};
    }

    // Actualizar estado de un nodo
    void update_node_health(NodeId id, NodeStatus status, double load = 0.0) {
        std::unique_lock lock(nodes_mutex_);
        if (id < nodes_.size()) {
            nodes_[id].status = status;
            nodes_[id].load_factor = load;
            nodes_[id].last_heartbeat = std::chrono::steady_clock::now();
        }
    }

    // Re-routing cuando un nodo falla
    void handle_node_failure(NodeId failed_node) {
        std::unique_lock shard_lock(shard_mutex_);
        std::unique_lock node_lock(nodes_mutex_);
        
        nodes_[failed_node].status = NodeStatus::OFFLINE;
        
        // Promover réplicas a primarios para shards afectados
        for (auto& assignment : shard_map_) {
            if (assignment.primary == failed_node && !assignment.replicas.empty()) {
                // Promover primera réplica a primario
                assignment.primary = assignment.replicas[0];
                assignment.replicas.erase(assignment.replicas.begin());
            }
        }
    }

    // Versioning para consistencia causal
    Version get_local_version() const {
        return local_version_.load(std::memory_order_acquire);
    }

    Version increment_version() {
        return local_version_.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

    void update_version_vector(NodeId node, Version v) {
        std::lock_guard lock(pending_mutex_);
        version_vector_[node] = std::max(version_vector_[node], v);
    }

    // Configuración
    const ClusterConfig& get_config() const { return config_; }
    NodeId get_local_node_id() const { return local_node_id_; }
    size_t get_total_shards() const { return shard_map_.size(); }

private:
    size_t get_shard(const Key& key) const {
        size_t h = std::hash<Key>{}(key);
        // Murmur3 finalizer
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        return h % shard_map_.size();
    }
};

// -----------------------------------------------------------------------------
// Distributed AVL Node (wrapper del árbol local)
// -----------------------------------------------------------------------------

template<typename Key, typename Value>
class DistributedAVLNode {
private:
    NodeId node_id_;
    AVLTreeParallelV2<Key, Value> local_tree_;
    std::shared_ptr<DistributedCoordinator<Key, Value>> coordinator_;
    std::shared_ptr<ITransport<Key, Value>> transport_;
    
    // Request queue para operaciones remotas
    struct RemoteRequest {
        Message<Key, Value> msg;
        std::promise<std::optional<Value>> promise;
    };
    std::queue<RemoteRequest> request_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    
    std::atomic<bool> running_{false};
    std::thread worker_thread_;

public:
    DistributedAVLNode(
        NodeId id,
        size_t local_shards,
        std::shared_ptr<DistributedCoordinator<Key, Value>> coordinator,
        std::shared_ptr<ITransport<Key, Value>> transport = nullptr)
        : node_id_(id)
        , local_tree_(local_shards)
        , coordinator_(coordinator)
        , transport_(transport)
    {
        local_tree_.set_prediction_enabled(true);
    }

    ~DistributedAVLNode() {
        stop();
    }

    void start() {
        running_ = true;
        worker_thread_ = std::thread([this] { worker_loop(); });
    }

    void stop() {
        running_ = false;
        queue_cv_.notify_all();
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
    }

    // API pública - operaciones distribuidas
    
    bool insert(const Key& key, const Value& value) {
        if (coordinator_->is_local(key)) {
            // Operación local
            local_tree_.insert(key, value);
            
            // Replicar si es necesario
            if (coordinator_->get_config().replication_factor > 1) {
                replicate_write(key, value);
            }
            return true;
        } else {
            // Forward a nodo correcto
            return forward_insert(key, value);
        }
    }

    std::optional<Value> get(const Key& key) {
        if (coordinator_->is_local(key)) {
            if (local_tree_.contains(key)) {
                return local_tree_.get(key);
            }
            return std::nullopt;
        } else {
            return forward_get(key);
        }
    }

    bool remove(const Key& key) {
        if (coordinator_->is_local(key)) {
            local_tree_.remove(key);
            
            if (coordinator_->get_config().replication_factor > 1) {
                replicate_remove(key);
            }
            return true;
        } else {
            return forward_remove(key);
        }
    }

    bool contains(const Key& key) {
        if (coordinator_->is_local(key)) {
            return local_tree_.contains(key);
        } else {
            auto result = forward_get(key);
            return result.has_value();
        }
    }

    // Acceso al árbol local
    AVLTreeParallelV2<Key, Value>& get_local_tree() { return local_tree_; }
    const AVLTreeParallelV2<Key, Value>& get_local_tree() const { return local_tree_; }

    NodeId get_node_id() const { return node_id_; }

private:
    void worker_loop() {
        while (running_) {
            std::unique_lock lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { 
                return !request_queue_.empty() || !running_; 
            });
            
            if (!running_) break;
            
            if (!request_queue_.empty()) {
                auto request = std::move(request_queue_.front());
                request_queue_.pop();
                lock.unlock();
                
                process_request(request);
            }
        }
    }

    void process_request(RemoteRequest& request) {
        // Procesar según tipo de mensaje
        switch (request.msg.type) {
            case MessageType::INSERT:
                local_tree_.insert(request.msg.key, *request.msg.value);
                request.promise.set_value(std::nullopt);
                break;
                
            case MessageType::GET:
                if (local_tree_.contains(request.msg.key)) {
                    request.promise.set_value(local_tree_.get(request.msg.key));
                } else {
                    request.promise.set_value(std::nullopt);
                }
                break;
                
            case MessageType::REMOVE:
                local_tree_.remove(request.msg.key);
                request.promise.set_value(std::nullopt);
                break;
                
            default:
                request.promise.set_value(std::nullopt);
        }
    }

    void replicate_write(const Key& key, const Value& value) {
        if (!transport_) return;
        
        auto replicas = coordinator_->get_replica_nodes(key);
        Message<Key, Value> msg;
        msg.type = MessageType::REPLICATE;
        msg.source = node_id_;
        msg.key = key;
        msg.value = value;
        msg.version = coordinator_->increment_version();
        
        for (NodeId replica : replicas) {
            transport_->send(replica, msg);
        }
    }

    void replicate_remove(const Key& key) {
        if (!transport_) return;
        
        auto replicas = coordinator_->get_replica_nodes(key);
        Message<Key, Value> msg;
        msg.type = MessageType::REMOVE;
        msg.source = node_id_;
        msg.key = key;
        msg.version = coordinator_->increment_version();
        
        for (NodeId replica : replicas) {
            transport_->send(replica, msg);
        }
    }

    bool forward_insert(const Key& key, const Value& value) {
        if (!transport_) return false;
        
        NodeId target = coordinator_->get_primary_node(key);
        Message<Key, Value> msg;
        msg.type = MessageType::INSERT;
        msg.source = node_id_;
        msg.target = target;
        msg.key = key;
        msg.value = value;
        
        return transport_->send(target, msg);
    }

    std::optional<Value> forward_get(const Key& key) {
        if (!transport_) return std::nullopt;
        
        NodeId target = coordinator_->get_primary_node(key);
        Message<Key, Value> msg;
        msg.type = MessageType::GET;
        msg.source = node_id_;
        msg.target = target;
        msg.key = key;
        
        // En una implementación real, esperaríamos la respuesta
        // Por ahora, solo enviamos y retornamos nullopt
        transport_->send(target, msg);
        return std::nullopt;
    }

    bool forward_remove(const Key& key) {
        if (!transport_) return false;
        
        NodeId target = coordinator_->get_primary_node(key);
        Message<Key, Value> msg;
        msg.type = MessageType::REMOVE;
        msg.source = node_id_;
        msg.target = target;
        msg.key = key;
        
        return transport_->send(target, msg);
    }
};

// -----------------------------------------------------------------------------
// Cluster Manager (helper para crear clusters de testing)
// -----------------------------------------------------------------------------

template<typename Key, typename Value>
class ClusterManager {
private:
    ClusterConfig config_;
    std::shared_ptr<LocalTransport<Key, Value>> transport_;
    std::shared_ptr<DistributedCoordinator<Key, Value>> coordinator_;
    std::vector<std::unique_ptr<DistributedAVLNode<Key, Value>>> nodes_;

public:
    explicit ClusterManager(const ClusterConfig& config)
        : config_(config)
        , transport_(std::make_shared<LocalTransport<Key, Value>>())
    {
        // Crear coordinador
        coordinator_ = std::make_shared<DistributedCoordinator<Key, Value>>(
            config_, 0, transport_
        );
        
        // Crear nodos
        for (size_t i = 0; i < config_.num_nodes; ++i) {
            auto node = std::make_unique<DistributedAVLNode<Key, Value>>(
                static_cast<NodeId>(i),
                config_.shards_per_node,
                coordinator_,
                transport_
            );
            
            // Registrar handler para este nodo
            transport_->register_node(static_cast<NodeId>(i), 
                [&node = *node](const Message<Key, Value>& msg) {
                    // En una implementación real, procesaríamos el mensaje
                });
            
            nodes_.push_back(std::move(node));
        }
    }

    void start_all() {
        for (auto& node : nodes_) {
            node->start();
        }
    }

    void stop_all() {
        for (auto& node : nodes_) {
            node->stop();
        }
    }

    DistributedAVLNode<Key, Value>* get_node(NodeId id) {
        if (id < nodes_.size()) {
            return nodes_[id].get();
        }
        return nullptr;
    }

    DistributedCoordinator<Key, Value>* get_coordinator() {
        return coordinator_.get();
    }

    size_t get_num_nodes() const { return nodes_.size(); }

    // Operaciones a nivel de cluster (routing automático)
    bool insert(const Key& key, const Value& value) {
        NodeId target = coordinator_->get_primary_node(key);
        return nodes_[target]->insert(key, value);
    }

    std::optional<Value> get(const Key& key) {
        NodeId target = coordinator_->get_primary_node(key);
        return nodes_[target]->get(key);
    }

    bool remove(const Key& key) {
        NodeId target = coordinator_->get_primary_node(key);
        return nodes_[target]->remove(key);
    }

    // Stats del cluster
    struct ClusterStats {
        size_t total_elements;
        std::vector<size_t> elements_per_node;
        double balance_score;
    };

    ClusterStats get_stats() const {
        ClusterStats stats;
        stats.total_elements = 0;
        
        for (const auto& node : nodes_) {
            size_t count = node->get_local_tree().size();
            stats.elements_per_node.push_back(count);
            stats.total_elements += count;
        }
        
        // Calcular balance
        if (!nodes_.empty() && stats.total_elements > 0) {
            double avg = stats.total_elements / static_cast<double>(nodes_.size());
            double variance = 0;
            for (size_t count : stats.elements_per_node) {
                double diff = count - avg;
                variance += diff * diff;
            }
            double std_dev = std::sqrt(variance / nodes_.size());
            stats.balance_score = std::max(0.0, 1.0 - (std_dev / avg));
        } else {
            stats.balance_score = 1.0;
        }
        
        return stats;
    }
};

} // namespace distributed

#endif // DISTRIBUTED_AVL_HPP
