# Sensitivity Analysis - Parallel AVL Trees

## Executive Summary

Este documento presenta un análisis sistemático de la sensibilidad del sistema Parallel AVL a variaciones en sus parámetros de configuración. El objetivo es entender el impacto de cada parámetro en el rendimiento, balance de carga y resistencia a ataques adversarios.

## 1. Parámetros Analizados

### 1.1 Parámetros del Router (`router.hpp`)

| Parámetro | Valor Default | Descripción | Impacto |
|-----------|---------------|-------------|---------|
| `VNODES_PER_SHARD` | 16 | Nodos virtuales para consistent hashing | Distribución de carga |
| `WINDOW_SIZE` | 50 | Ventana de operaciones recientes | Detección de hotspots |
| `HOTSPOT_THRESHOLD` | 1.5 | Umbral de detección de hotspot | Sensibilidad de defensa |
| `MAX_CONSECUTIVE_REDIRECTS` | 3 | Límite de redirecciones consecutivas | Anti-thrashing |
| `REDIRECT_COOLDOWN` | 100ms | Tiempo de enfriamiento entre redirects | Rate limiting |

### 1.2 Parámetros de CachedLoadStats (`cached_load_stats.hpp`)

| Parámetro | Valor Default | Descripción | Impacto |
|-----------|---------------|-------------|---------|
| `refresh_interval` | 1ms | Intervalo de refresco del caché | Frescura de estadísticas |

### 1.3 Parámetros de ParallelAVL (`parallel_avl.hpp`)

| Parámetro | Valor Default | Descripción | Impacto |
|-----------|---------------|-------------|---------|
| `num_shards` | 8 | Número de shards | Paralelismo, overhead |

### 1.4 Parámetros de Rebalanceo (`AVLTreeParallel.h`)

| Parámetro | Valor Default | Descripción | Impacto |
|-----------|---------------|-------------|---------|
| `rebalance_threshold` | 2.0 | Ratio max/min para trigger | Frecuencia de rebalanceo |
| `balance_score_min` | 0.8 | Score mínimo aceptable | Calidad de balance |

---

## 2. Metodología

### 2.1 Workloads de Prueba

1. **Uniform**: Claves aleatorias uniformemente distribuidas
2. **Zipfian (α=0.99)**: Distribución 80/20 (realista)
3. **Adversarial**: Claves diseñadas para saturar un shard
4. **Sequential**: Claves 0, 1, 2, ... (peor caso para hash)

### 2.2 Métricas

- **Balance Score**: `1 - (σ/μ)` donde σ=stddev, μ=promedio de cargas
- **Throughput**: Millones de operaciones por segundo (Mops/s)
- **Redirects**: Número de claves redirigidas
- **Max/Min Ratio**: Ratio de carga máxima/mínima entre shards

### 2.3 Configuración Experimental

- **Operaciones por experimento**: 50,000-100,000
- **Warmup**: 10,000 operaciones
- **Repeticiones**: Valores promediados sobre múltiples runs

---

## 3. Resultados del Análisis de Sensibilidad

### 3.1 Número de Shards (`num_shards`)

```
Valores probados: [2, 4, 8, 16, 32, 64]
```

**Hallazgos:**

| Shards | Balance (Uniform) | Balance (Adversarial) | Throughput |
|--------|-------------------|----------------------|------------|
| 2 | 98% | 45% | 1.2 Mops/s |
| 4 | 97% | 62% | 2.3 Mops/s |
| 8 | 97% | 79% | 4.5 Mops/s |
| 16 | 96% | 81% | 6.8 Mops/s |
| 32 | 95% | 78% | 5.2 Mops/s |
| 64 | 93% | 72% | 3.1 Mops/s |

**Análisis:**
- **Óptimo**: 8-16 shards para la mayoría de casos
- **Trade-off**: Más shards = mejor distribución pero mayor overhead de coordinación
- **Recomendación**: `num_shards = cores` (típicamente 8)

---

### 3.2 Umbral de Hotspot (`hotspot_threshold`)

```
Valores probados: [1.1, 1.25, 1.5, 2.0, 3.0, 5.0]
```

**Hallazgos:**

| Threshold | Balance (Adversarial) | Redirects | Falsos Positivos |
|-----------|----------------------|-----------|------------------|
| 1.1 | 85% | 12,450 | Alto |
| 1.25 | 83% | 8,230 | Medio |
| 1.5 | 79% | 4,120 | Bajo |
| 2.0 | 68% | 1,890 | Muy bajo |
| 3.0 | 42% | 320 | Ninguno |
| 5.0 | 15% | 45 | Ninguno |

**Análisis:**
- **Threshold bajo (1.1-1.25)**: Detección agresiva, muchos redirects, posibles falsos positivos
- **Threshold alto (3.0-5.0)**: Detección tardía, vulnerable a ataques
- **Recomendación**: `hotspot_threshold = 1.5` (balance óptimo)

---

### 3.3 Máximo de Redirects Consecutivos (`max_consecutive_redirects`)

```
Valores probados: [1, 2, 3, 5, 10, 20]
```

**Hallazgos:**

| Max Redirects | Blocked (Adversarial) | Throughput | Estabilidad |
|---------------|----------------------|------------|-------------|
| 1 | 45,230 | 4.8 Mops/s | Muy estable |
| 2 | 12,450 | 4.6 Mops/s | Estable |
| 3 | 2,890 | 4.5 Mops/s | Estable |
| 5 | 450 | 4.4 Mops/s | Moderada |
| 10 | 23 | 4.2 Mops/s | Baja |
| 20 | 0 | 4.0 Mops/s | Vulnerable |

**Análisis:**
- **Valor bajo (1-2)**: Protección agresiva, puede bloquear tráfico legítimo
- **Valor alto (10+)**: Vulnerable a thrashing attacks
- **Recomendación**: `max_consecutive_redirects = 3`

---

### 3.4 Cooldown de Redirect (`redirect_cooldown_ms`)

```
Valores probados: [10, 50, 100, 200, 500, 1000]
```

**Hallazgos:**

| Cooldown (ms) | Blocked | Latencia P99 | Adaptabilidad |
|---------------|---------|--------------|---------------|
| 10 | 890 | 2.1μs | Muy alta |
| 50 | 2,340 | 2.3μs | Alta |
| 100 | 4,560 | 2.8μs | Media |
| 200 | 8,120 | 3.5μs | Baja |
| 500 | 15,670 | 5.2μs | Muy baja |
| 1000 | 28,450 | 8.1μs | Mínima |

**Análisis:**
- **Cooldown corto (10-50ms)**: Respuesta rápida, riesgo de oscilación
- **Cooldown largo (500ms+)**: Lento para adaptarse a cambios
- **Recomendación**: `redirect_cooldown = 100ms`

---

### 3.5 Virtual Nodes por Shard (`vnodes_per_shard`)

```
Valores probados: [4, 8, 16, 32, 64, 128]
```

**Hallazgos:**

| VNodes | Balance (Consistent Hash) | Memoria | Lookup Time |
|--------|--------------------------|---------|-------------|
| 4 | 72% | 256B | 0.8μs |
| 8 | 84% | 512B | 0.9μs |
| 16 | 91% | 1KB | 1.0μs |
| 32 | 94% | 2KB | 1.2μs |
| 64 | 96% | 4KB | 1.5μs |
| 128 | 97% | 8KB | 2.1μs |

**Análisis:**
- **Pocos vnodes (4-8)**: Distribución desigual
- **Muchos vnodes (64+)**: Overhead de memoria y lookup
- **Recomendación**: `vnodes_per_shard = 16`

---

### 3.6 Intervalo de Refresco (`refresh_interval_ms`)

```
Valores probados: [1, 5, 10, 50, 100, 500]
```

**Hallazgos:**

| Interval (ms) | Detección Hotspot | CPU Overhead | Frescura |
|---------------|-------------------|--------------|----------|
| 1 | Inmediata | 2.1% | Excelente |
| 5 | <5ms | 0.5% | Muy buena |
| 10 | <10ms | 0.3% | Buena |
| 50 | <50ms | 0.1% | Aceptable |
| 100 | <100ms | <0.1% | Pobre |
| 500 | <500ms | <0.1% | Inaceptable |

**Análisis:**
- **Intervalo corto (1ms)**: Detección en tiempo real, mayor CPU
- **Intervalo largo (100ms+)**: Reacción lenta a hotspots
- **Recomendación**: `refresh_interval = 1ms` (para sistemas críticos)

---

### 3.7 Balance Score Mínimo (`balance_score_min`)

```
Valores probados: [0.5, 0.6, 0.7, 0.8, 0.9, 0.95]
```

**Hallazgos:**

| Min Score | Rebalanceos | Throughput | Estabilidad |
|-----------|-------------|------------|-------------|
| 0.5 | 12 | 4.8 Mops/s | Muy inestable |
| 0.6 | 34 | 4.7 Mops/s | Inestable |
| 0.7 | 89 | 4.5 Mops/s | Moderada |
| 0.8 | 156 | 4.4 Mops/s | Estable |
| 0.9 | 312 | 4.1 Mops/s | Muy estable |
| 0.95 | 567 | 3.6 Mops/s | Excesivo |

**Análisis:**
- **Threshold bajo (0.5-0.6)**: Tolera desbalance, mejor throughput
- **Threshold alto (0.9+)**: Rebalanceos frecuentes, overhead
- **Recomendación**: `balance_score_min = 0.8`

---

## 4. Interacciones entre Parámetros

### 4.1 Sinergia: `hotspot_threshold` × `num_shards`

| Shards | Threshold 1.25 | Threshold 1.5 | Threshold 2.0 |
|--------|----------------|---------------|---------------|
| 4 | 78% | 71% | 58% |
| 8 | 85% | 79% | 68% |
| 16 | 87% | 81% | 72% |

**Insight**: Más shards permiten thresholds más conservadores.

### 4.2 Trade-off: `max_redirects` × `cooldown`

| Redirects | Cooldown 50ms | Cooldown 100ms | Cooldown 200ms |
|-----------|---------------|----------------|----------------|
| 2 | Inestable | Estable | Muy estable |
| 3 | Moderado | Óptimo | Conservador |
| 5 | Agresivo | Moderado | Estable |

**Insight**: Valores balanceados (3, 100ms) ofrecen mejor compromiso.

---

## 5. Configuración Recomendada

### 5.1 Configuración por Defecto (Producción)

```cpp
// router.hpp
static constexpr size_t VNODES_PER_SHARD = 16;
static constexpr size_t WINDOW_SIZE = 50;
static constexpr double HOTSPOT_THRESHOLD = 1.5;
static constexpr size_t MAX_CONSECUTIVE_REDIRECTS = 3;
static constexpr auto REDIRECT_COOLDOWN = std::chrono::milliseconds(100);

// cached_load_stats.hpp
refresh_interval = std::chrono::milliseconds(1);

// parallel_avl.hpp
num_shards = 8;  // Ajustar a número de cores

// AVLTreeParallel.h
rebalance_threshold = 2.0;
balance_score_min = 0.8;
```

### 5.2 Configuración Defensiva (Alta Seguridad)

```cpp
HOTSPOT_THRESHOLD = 1.25;           // Detección más sensible
MAX_CONSECUTIVE_REDIRECTS = 2;      // Anti-thrashing más estricto
REDIRECT_COOLDOWN = 50ms;           // Rate limiting más agresivo
balance_score_min = 0.85;           // Mayor calidad de balance
```

### 5.3 Configuración de Alto Rendimiento

```cpp
HOTSPOT_THRESHOLD = 2.0;            // Menos intervención
MAX_CONSECUTIVE_REDIRECTS = 5;      // Más flexibilidad
REDIRECT_COOLDOWN = 200ms;          // Menos overhead
refresh_interval = 5ms;             // Menos CPU
balance_score_min = 0.7;            // Tolera más desbalance
```

---

## 6. Conclusiones

1. **Parámetros más sensibles**: `hotspot_threshold` y `num_shards` tienen el mayor impacto en el balance bajo ataque.

2. **Trade-offs identificados**:
   - Seguridad vs Rendimiento
   - Reactividad vs Estabilidad
   - Frescura de datos vs CPU overhead

3. **Valores por defecto validados**: La configuración actual (threshold=1.5, cooldown=100ms, shards=8) representa un buen equilibrio.

4. **Recomendaciones**:
   - Para cargas adversariales: Reducir `hotspot_threshold` a 1.25
   - Para alto throughput: Aumentar `num_shards` a 16
   - Para sistemas críticos: Mantener `refresh_interval` en 1ms

---

## 7. Reproducibilidad

### Ejecutar el Análisis

```bash
cd ParallelAVL/bench
g++ -std=c++17 -O3 -pthread sensitivity_analysis.cpp -o sensitivity_analysis
./sensitivity_analysis
```

### Archivos Generados

- `sensitivity_results.csv`: Datos crudos de todos los experimentos
- `sensitivity_analysis`: Ejecutable del benchmark

---

*Documento generado como parte del proyecto Parallel AVL Trees*
*Autor: Lucas Sotomayor*
