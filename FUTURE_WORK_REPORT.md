# Reporte de Trabajos Futuros - Parallel AVL Tree

**Fecha**: 29 de Diciembre, 2025  
**Autor**: Lucas Sotomayor  
**Proyecto**: Árboles AVL Paralelos con Arquitectura Sharded

---

## Resumen Ejecutivo

Este documento presenta los resultados de la implementación y evaluación de cuatro líneas de trabajo futuro propuestas para el proyecto de Árboles AVL Paralelos:

1. **Read-Copy-Update (RCU) / shared_mutex** - ❌ No recomendado
2. **Routing Predictivo ML-lite** - ⚠️ Redundante con hash robusto
3. **Shards Dinámicos** - ✅ **Exitoso y recomendado**
4. **Extensión Distribuida** - 📋 Framework diseñado, pendiente implementación

---

## 1. shared_mutex para Lecturas Concurrentes

### Hipótesis
> "Reemplazar `std::mutex` por `std::shared_mutex` permitirá lecturas paralelas, mejorando el rendimiento en cargas read-heavy."

### Resultados

| Implementación | Tiempo (600K ops) | Throughput |
|----------------|-------------------|------------|
| V1 (std::mutex) | 31.68 ms | **18.9M ops/s** |
| V2 (std::shared_mutex) | 1086.24 ms | 552K ops/s |

**Resultado: 34× más lento con shared_mutex**

### Análisis

El modelo mental "más readers en paralelo = mejor" asume que el costo de la operación protegida domina. En nuestro caso:

```
Tiempo total = T_lock + T_operación + T_unlock

std::mutex:        ~50ns + ~300ns + ~10ns  = ~360ns
std::shared_mutex: ~400ns + ~300ns + ~150ns = ~850ns
```

El overhead de `shared_mutex` (coordinación de reader count atómico) **excede el costo de la operación AVL**.

### Lección Aprendida
`shared_mutex` solo conviene cuando:
- Operaciones protegidas son costosas (>10μs): I/O, red, cálculos complejos
- Ratio de lecturas >99% con muy pocos writers

### Recomendación
**No implementar** para operaciones en memoria pura. Mantener `std::mutex` simple.

---

## 2. Routing Predictivo (ML-lite)

### Hipótesis
> "Un predictor basado en EMA (Exponential Moving Average) puede detectar hotspots antes de que ocurran y redistribuir proactivamente."

### Resultados

| Estrategia | Throughput | Balance | Hotspots Detectados |
|------------|------------|---------|---------------------|
| Static Hash | 3.4M ops/s | 99.5% | N/A |
| Predictive (EMA) | 2.4M ops/s | 99.5% | 0 |

**Resultado: 29% más lento, sin beneficio**

### Análisis

El predictor no detectó hotspots porque nuestro hash robusto (Murmur3 finalizer) distribuye **demasiado bien**:

```cpp
h ^= h >> 33;
h *= 0xff51afd7ed558ccdULL;  // Efecto avalancha
h ^= h >> 33;
```

Incluso con distribución Zipf (sesgada), el hash la "uniformiza".

### Lección Aprendida
El routing predictivo es útil **solo si**:
- El hash permite hotspots (hash débil o identity hash)
- Hay ataques adversariales reales
- El patrón de acceso es predecible temporalmente

### Recomendación
**No implementar**. El router adversary-resistant existente (`router.hpp`) ya maneja estos casos de forma más eficiente.

---

## 3. Shards Dinámicos con Consistent Hashing ✅

### Hipótesis
> "Usando consistent hashing y migración activa, se puede escalar horizontalmente sin perder balance."

### Resultados

| Fase | AVLTreeParallelV2 (sin migración) | DynamicShardedTree (con migración) |
|------|-----------------------------------|-----------------------------------|
| 4 shards | 84.6% balance | 82.9% balance |
| 6 shards | 62.5% balance | 89.1% balance |
| 8 shards | **52.2% balance** ⚠️ | **87.7% balance** ✓ |

**Throughput comparativo:**
- AVLTreeParallelV2: 735K ops/s
- DynamicShardedTree: **1.05M ops/s** (+43%)

**Velocidad de rebalance:** 3.5M keys/sec

### Análisis

El problema con el enfoque anterior era que al agregar shards:
```
hash(key) % 4 = 2  →  Shard 2
hash(key) % 6 = 4  →  Shard 4  (¡pero el dato está en Shard 2!)
```

Sin migración, los datos viejos quedan desbalanceados.

**Solución implementada:**
1. **Consistent Hash Ring** con 64 virtual nodes por shard
2. **Migración Lazy** durante operaciones `contains()`/`get()`
3. **`force_rebalance()`** para redistribución completa cuando sea necesario

### Implementación

```cpp
// include/DynamicShardedTree.hpp
template<typename Key, typename Value = Key>
class DynamicShardedTree {
    // Consistent hash ring
    std::vector<VirtualNode> hash_ring_;
    
    // Scaling dinámico
    void add_shard();
    void remove_shard();
    void force_rebalance();
    
    // Migración lazy integrada en contains()/get()
};
```

### Recomendación
**Implementar y usar** como reemplazo de AVLTreeParallel para casos que requieran escalado elástico.

---

## 4. Extensión Distribuida

### Estado
Framework diseñado, implementación parcial como stub.

### Componentes Diseñados

```
         [Client Layer]
               |
        [Router/Coordinator]
               |
   +------+----+----+------+
   |      |         |      |
[Node 0] [Node 1] [Node 2] [Node 3]
Shards   Shards   Shards   Shards
0-7      8-15     16-23    24-31
```

- `DistributedCoordinator`: Routing entre nodos, health tracking
- `DistributedAVLNode`: Wrapper del árbol local con operaciones remotas
- `ClusterManager`: Helper para crear clusters de testing
- Soporte para consistencia STRONG/EVENTUAL/CAUSAL

### Lo que Falta
1. **Transport real**: gRPC, TCP sockets, o similar
2. **Consensus protocol**: Raft/Paxos para consistencia fuerte
3. **Failure handling**: Detección de particiones, recuperación
4. **Replicación**: Implementación completa de sync/async

### Recomendación
**Posponer** hasta que haya un caso de uso real que justifique la complejidad.

---

## Plan de Integración: DynamicShardedTree

### Fase 1: Preparación (Día 1)

#### 1.1 Verificar compatibilidad de API

```cpp
// API actual (AVLTreeParallel)
tree.insert(key, value);
tree.contains(key);
tree.get(key);
tree.remove(key);
tree.size();

// API nueva (DynamicShardedTree) - COMPATIBLE ✓
tree.insert(key, value);
tree.contains(key);
tree.get(key);
tree.remove(key);
tree.size();

// Métodos adicionales
tree.add_shard();           // Nuevo: escalar horizontalmente
tree.remove_shard();        // Nuevo: reducir shards
tree.force_rebalance();     // Nuevo: redistribuir datos
tree.get_stats();           // Nuevo: estadísticas detalladas
tree.print_stats();         // Nuevo: debug
```

#### 1.2 Crear alias para migración gradual

Agregar a `include/ParallelAVL.hpp` (nuevo archivo):

```cpp
#ifndef PARALLEL_AVL_HPP
#define PARALLEL_AVL_HPP

// Opción de compilación para elegir implementación
#ifdef USE_DYNAMIC_SHARDS
    #include "DynamicShardedTree.hpp"
    template<typename K, typename V = K>
    using ParallelAVL = DynamicShardedTree<K, V>;
#else
    #include "AVLTreeParallel.h"
    template<typename K, typename V = K>
    using ParallelAVL = AVLTreeParallel<K, V>;
#endif

#endif
```

### Fase 2: Migración de Tests (Día 2)

#### 2.1 Actualizar tests existentes

```cpp
// Antes
#include "AVLTreeParallel.h"
AVLTreeParallel<int, int> tree(8);

// Después
#include "ParallelAVL.hpp"
ParallelAVL<int, int> tree;  // Usa config por defecto
```

#### 2.2 Crear test de regresión

```cpp
// tests/regression_dynamic_shards.cpp
void test_api_compatibility() {
    DynamicShardedTree<int, int> tree;
    
    // Todas las operaciones básicas deben funcionar
    for (int i = 0; i < 10000; ++i) {
        tree.insert(i, i * 10);
    }
    
    assert(tree.size() == 10000);
    assert(tree.contains(5000));
    assert(tree.get(5000) == 50000);
    
    tree.remove(5000);
    assert(!tree.contains(5000));
    assert(tree.size() == 9999);
}

void test_scaling() {
    DynamicShardedTree<int, int> tree;
    
    // Insertar datos
    for (int i = 0; i < 50000; ++i) {
        tree.insert(i, i);
    }
    
    auto stats1 = tree.get_stats();
    assert(stats1.balance_score > 0.8);
    
    // Escalar
    tree.add_shard();
    tree.add_shard();
    tree.force_rebalance();
    
    auto stats2 = tree.get_stats();
    assert(stats2.balance_score > 0.8);
    assert(stats2.num_shards == stats1.num_shards + 2);
}
```

### Fase 3: Integración en Benchmarks (Día 3)

#### 3.1 Actualizar benchmark principal

```cpp
// benchmark_parallel_trees.cpp
#include "ParallelAVL.hpp"

// Compilar con:
// g++ -DUSE_DYNAMIC_SHARDS ... (usa DynamicShardedTree)
// g++ ...                      (usa AVLTreeParallel original)
```

#### 3.2 Agregar benchmark de scaling

```cpp
void benchmark_elastic_scaling() {
    DynamicShardedTree<int, int> tree;
    
    // Simular carga creciente
    for (int phase = 0; phase < 5; ++phase) {
        // Insertar datos
        insert_batch(tree, 100000);
        
        // Escalar si balance baja
        auto stats = tree.get_stats();
        if (stats.balance_score < 0.7) {
            tree.add_shard();
            tree.force_rebalance();
        }
        
        print_phase_stats(phase, stats);
    }
}
```

### Fase 4: Documentación (Día 4)

#### 4.1 Actualizar README.md

```markdown
## Uso Básico

### Árbol Paralelo Estático
```cpp
#include "AVLTreeParallel.h"
AVLTreeParallel<int, int> tree(8);  // 8 shards fijos
```

### Árbol Paralelo Dinámico (Recomendado)
```cpp
#include "DynamicShardedTree.hpp"

DynamicShardedTree<int, int>::Config config;
config.initial_shards = 4;
config.vnodes_per_shard = 64;

DynamicShardedTree<int, int> tree(config);

// Escalar cuando sea necesario
tree.add_shard();
tree.force_rebalance();
```
```

#### 4.2 Agregar ejemplos

```cpp
// examples/dynamic_scaling_example.cpp
#include "DynamicShardedTree.hpp"
#include <iostream>

int main() {
    DynamicShardedTree<std::string, int> cache;
    
    // Uso como cache
    cache.insert("user:1001", 42);
    cache.insert("user:1002", 37);
    
    // Escalar bajo demanda
    if (cache.size() > 100000) {
        cache.add_shard();
        cache.force_rebalance();
    }
    
    cache.print_stats();
    return 0;
}
```

### Fase 5: Deprecación Gradual (Semana 2+)

#### 5.1 Marcar AVLTreeParallel como legacy

```cpp
// AVLTreeParallel.h
#ifndef AVL_TREE_PARALLEL_H
#define AVL_TREE_PARALLEL_H

#warning "AVLTreeParallel is deprecated. Use DynamicShardedTree instead."

// ... código existente ...
```

#### 5.2 Timeline de deprecación

| Fecha | Acción |
|-------|--------|
| Semana 1 | Integrar DynamicShardedTree, tests pasan |
| Semana 2 | Agregar warning de deprecación |
| Semana 4 | Documentación completa migrada |
| Semana 8 | Remover AVLTreeParallel del código principal |

### Checklist de Integración

- [ ] Crear `include/ParallelAVL.hpp` con alias
- [ ] Actualizar tests para usar nueva API
- [ ] Crear test de regresión específico
- [ ] Actualizar benchmarks principales
- [ ] Actualizar README.md con ejemplos
- [ ] Agregar ejemplo de scaling dinámico
- [ ] Marcar AVLTreeParallel como deprecated
- [ ] Actualizar documentación del paper

---

## Próximos Pasos Recomendados

### Corto Plazo (1-2 semanas)

1. **Integrar DynamicShardedTree** como opción principal
   - Documentar API en README
   - Agregar ejemplos de uso
   - Tests de regresión

2. **Limpiar código experimental**
   - Remover AVLTreeParallelV2 (shared_mutex no aporta valor)
   - Remover PredictiveRouter (redundante)
   - Mantener solo DynamicShardedTree y router.hpp

3. **Benchmarks adicionales**
   - Probar con más shards (16, 32, 64)
   - Medir latencia p99 durante scaling
   - Comparar con otras estructuras (ConcurrentHashMap, etc.)

### Mediano Plazo (1-3 meses)

4. **Optimizaciones del DynamicShardedTree**
   - Migración background opcional (sin deadlocks)
   - Bulk rebalance para operaciones batch
   - Métricas de migración (keys movidas, tiempo, etc.)

5. **Documentación académica**
   - Paper sobre consistent hashing aplicado a árboles
   - Comparación con Redis Cluster, DynamoDB

### Largo Plazo (si hay demanda)

6. **Extensión distribuida completa**
   - Solo si hay caso de uso real que lo justifique
   - Considerar usar librerías existentes (gRPC, etcd) en lugar de reimplementar

---

## Archivos Creados

| Archivo | Descripción | Estado |
|---------|-------------|--------|
| `include/DynamicShardedTree.hpp` | Implementación robusta de shards dinámicos | ✅ Listo |
| `include/AVLTreeParallelV2.h` | Versión con shared_mutex | ❌ No recomendado |
| `include/PredictiveRouter.hpp` | Router ML-lite | ⚠️ Redundante |
| `include/DistributedAVL.hpp` | Framework distribuido | 📋 Stub |
| `bench/dynamic_shards_bench.cpp` | Benchmark de shards dinámicos | ✅ Listo |
| `bench/future_features_bench.cpp` | Benchmark de features V2 | ✅ Listo |
| `tests/distributed_test.cpp` | Tests del cluster | ✅ Listo |

---

## Conclusión

De las cuatro líneas de trabajo futuro evaluadas, **solo los Shards Dinámicos demostraron valor real**:

- **shared_mutex**: Contraproducente para operaciones rápidas
- **Routing predictivo**: Redundante con hash robusto existente
- **Shards dinámicos**: **+43% throughput, balance mantenido** ✅
- **Distribuido**: Complejidad no justificada sin caso de uso

La recomendación es **enfocarse en DynamicShardedTree** como la mejora principal del sistema, y posponer las demás líneas hasta que haya evidencia de necesidad real.

---

## Comandos Útiles

```bash
# Compilar todo
make future

# Ejecutar benchmark de shards dinámicos
./bench_dynamic_shards

# Ejecutar todos los benchmarks V2
make run-future
```

---

*Documento generado como parte del análisis de trabajos futuros del proyecto Parallel AVL Tree.*
