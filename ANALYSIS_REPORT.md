# ParallelAVL: Análisis de Rendimiento y Resistencia Adversarial

## Resumen Ejecutivo

**Hipótesis original**: GCC -O3 falla en arquitectura híbrida (P-cores + E-cores) pero ICX funciona.

**Conclusión**: No era bug de compilador ni arquitectura. Se identificaron y corrigieron **3 problemas de diseño**:

1. **Conteo inflado de cargas** - El router contaba intentos, no inserciones exitosas
2. **Hash inconsistente entre compiladores** - `std::hash<int>` es identity en GCC, scrambled en ICX
3. **Estrategia Intelligent ineficiente** - Llamaba `get_stats()` en cada routing

---

## Problemas Identificados y Soluciones

### Problema 1: Conteo Inflado de Cargas

**Síntoma**: Balance reportado 0% cuando la realidad era 62%

**Causa**: El router contaba todas las llamadas a `record_insertion()`, incluyendo duplicados.

```cpp
// ANTES (bug)
shards_[target_shard]->insert(key, value);  // Solo incrementa si key nueva
router_->record_insertion(target_shard);     // SIEMPRE incrementa ← BUG
```

**Fix implementado** en `include/parallel_avl.hpp`:

```cpp
// DESPUÉS (corregido)
size_t old_size = shards_[target_shard]->size();
shards_[target_shard]->insert(key, value);
size_t new_size = shards_[target_shard]->size();

if (new_size > old_size) {  // Solo si realmente se insertó
    router_->record_insertion(target_shard);
}
```

---

### Problema 2: Hash Inconsistente entre Compiladores

**Síntoma**: Ataque adversarial funcionaba en GCC pero no en ICX

**Causa**: `std::hash<int>` tiene diferente implementación:

| Compilador | `hash(0)` | `hash(8)` | Tipo |
|------------|-----------|-----------|------|
| GCC | 0 | 8 | Identity |
| ICX | 5558979... | 5546982... | Scrambled |

**Fix implementado** en `include/router.hpp` y `include/parallel_avl.hpp`:

```cpp
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
```

---

### Problema 3: Estrategia Intelligent Ineficiente

**Síntoma**: Intelligent 100x más lento que otras estrategias

**Causa**: Llamaba `get_stats()` (2 loops + sqrt) en cada routing

**Fix implementado** en `include/router.hpp`:

```cpp
// Cache adaptativo + fast-path
size_t route_intelligent(const Key& key, size_t natural_shard) {
    // Fast path: si está estable, bypasear todo el overhead
    if (adaptive_interval_.load() >= MAX_CACHE_INTERVAL) {
        return natural_shard;  // Mismo costo que STATIC_HASH
    }
    
    // Actualizar cache solo periódicamente (cada 10-500 ops)
    // Intervalo se adapta: menor bajo ataque, mayor cuando estable
    ...
}
```

---

## Resultados de Benchmarks

### Heavy Benchmark - Thread Scaling (8 shards, 100K ops/thread)

| Threads | Static Hash | Load-Aware | Intelligent |
|---------|-------------|------------|-------------|
| 1 | 4.12M ops/s | 2.70M ops/s | 2.05M ops/s |
| 8 | 1.94M ops/s | 1.83M ops/s | 1.61M ops/s |
| 16 | 1.73M ops/s | 1.62M ops/s | 1.77M ops/s |

**Balance**: 99%+ en todas las estrategias ✅

### Heavy Benchmark - Shard Scaling (8 threads)

| Shards | Static Hash | Load-Aware | Intelligent |
|--------|-------------|------------|-------------|
| 4 | 989K ops/s | 1.10M ops/s | 986K ops/s |
| 16 | 2.48M ops/s | 2.67M ops/s | 2.62M ops/s |
| 32 | 3.41M ops/s | 3.32M ops/s | 2.95M ops/s |

### Mixed Workload (80% reads, 20% writes)

| Strategy | Throughput | Balance | Latency |
|----------|------------|---------|---------|
| Static Hash | 3.32M ops/s | 99.2% | 0.30 μs |
| Load-Aware | 3.37M ops/s | 99.3% | 0.30 μs |
| Intelligent | 3.73M ops/s | 99.2% | 0.27 μs |

---

## Resistencia Adversarial

### Ataque con Weak Hash (vulnerable)

| Strategy | Balance bajo ataque | Distribución |
|----------|---------------------|--------------|
| Static Hash | **0%** ❌ | [5000, 0, 0, 0, 0, 0, 0, 0] |
| Load-Aware | **81%** ✅ | [4902, 3035, 3035, 3035, ...] |
| Intelligent | **81%** ✅ | [4902, 3035, 3035, 3035, ...] |

### Ataque con Robust Hash (protegido)

| Strategy | Balance bajo ataque | Distribución |
|----------|---------------------|--------------|
| Static Hash | **96%** ✅ | [591, 633, 626, 655, 658, 584, 623, 630] |
| Load-Aware | **96%** ✅ | [594, 637, 629, 657, 661, 586, 627, 634] |
| Intelligent | **96%** ✅ | [594, 637, 629, 657, 661, 586, 627, 634] |

### Consistencia GCC vs ICX

| Test | GCC | ICX |
|------|-----|-----|
| Weak Hash + Static | 0% | 0% |
| Weak Hash + Load-Aware | 81.1% | 81.1% |
| Robust Hash + Static | 96.0% | 96.0% |
| Robust Hash + Load-Aware | 96.1% | 96.1% |

**Ambos compiladores producen resultados idénticos** ✅

---

## Comparación con Optimizaciones Agresivas

### Flags de compilación

| Compilador | Flags |
|------------|-------|
| GCC | `-O3 -march=native -flto -ffast-math -funroll-loops` |
| ICX | `/O3 /QxHost /Qipo /fp:fast /Qunroll` |

### Resultados

| Métrica | GCC Aggressive | ICX Aggressive |
|---------|----------------|----------------|
| Single-thread | 3.69M ops/s | 2.66M ops/s |
| 16 threads | 1.56M ops/s | 1.72M ops/s |
| Mixed workload | 2.60M ops/s | 5.10M ops/s |
| Balance bajo ataque | 95% | 96% |

---

## Estrategias de Routing

### Static Hash
- **Uso**: Entornos trusted, máximo throughput
- **Fortaleza**: O(1), sin overhead
- **Debilidad**: Vulnerable a ataques de colisión

### Load-Aware
- **Uso**: Entornos con posibles ataques
- **Fortaleza**: Detecta y redistribuye hotspots
- **Debilidad**: Overhead en cada operación

### Intelligent (Adaptativo)
- **Uso**: Producción general
- **Fortaleza**: Se adapta automáticamente
- **Comportamiento**:
  - Estable → Fast-path (costo = Static Hash)
  - Bajo ataque → Load-Aware (protección)

---

## Archivos Modificados

| Archivo | Cambio |
|---------|--------|
| `include/parallel_avl.hpp` | Fix conteo + hash robusto |
| `include/router.hpp` | Hash robusto + Intelligent optimizado |

## Benchmarks Creados

| Archivo | Propósito |
|---------|-----------|
| `bench/heavy_fixed_bench.cpp` | Benchmark de throughput y escalabilidad |
| `bench/adversarial_bench.cpp` | Pruebas de resistencia a ataques |
| `bench/weak_hash_attack.cpp` | Comparación GCC vs ICX con hash débil |
| `bench/adversarial_murmur_attack.cpp` | Ataques contra hash robusto |

---

## Conclusión

| Hipótesis Original | Resultado |
|--------------------|-----------|
| Bug de GCC -O3 | ❌ No |
| Bug de arquitectura híbrida | ❌ No |
| Bug de memory ordering | ❌ No |
| **Bugs de diseño** | ✅ **3 identificados y corregidos** |

### Estado Final

- ✅ Balance 99%+ en operaciones normales
- ✅ Balance 81-96% bajo ataques adversariales
- ✅ Throughput hasta 4.12M ops/s
- ✅ Comportamiento idéntico GCC vs ICX
- ✅ Funciona en arquitecturas híbridas y Xeon
