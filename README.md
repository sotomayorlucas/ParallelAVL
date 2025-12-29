# Parallel AVL Tree

A high-performance concurrent AVL tree implementation with adaptive routing and linearizability guarantees.

## Overview

This project implements a **tree-of-trees** architecture for concurrent AVL operations with:
- **Near-linear scalability**: 7.78× speedup on 8 cores
- **Adaptive routing**: Prevents hotspots before they happen
- **Linearizability guarantees**: Thread-safe operations
- **Self-healing**: Automatic load balancing

## Project Structure

```
ParallelAVL/
├── include/           # Header files
│   ├── parallel_avl.hpp      # Main ParallelAVL class
│   ├── shard.hpp             # TreeShard container
│   ├── router.hpp            # Adversary-resistant routing
│   ├── redirect_index.hpp    # Key redirection tracking
│   ├── cached_load_stats.hpp # Load statistics cache
│   ├── workloads.hpp         # Workload generators
│   ├── AVLTree.h             # Base AVL tree
│   ├── AVLTreeParallel.h     # Parallel tree wrapper
│   └── AdaptiveRouter.h      # Adaptive routing system
├── bench/             # Benchmarks
│   ├── rigorous_bench.cpp    # Comprehensive benchmarks
│   ├── throughput_bench.cpp  # Throughput testing
│   └── adversarial_bench.cpp # Attack resistance tests
├── tests/             # Unit tests
│   ├── linearizability_test.cpp
│   └── workloads_test.cpp
├── paper/             # Academic paper
│   ├── rigorous_parallel_trees.tex
│   └── Makefile
├── benchmark_parallel_trees.cpp
├── benchmark_routing_strategies.cpp
├── PARALLEL_TREES_ARCHITECTURE.md
├── PAPER_README.md
└── README_PAPER.md
```

## Quick Start

### Build

```bash
# Compile benchmarks
g++ -std=c++17 -O3 -pthread -I include benchmark_parallel_trees.cpp -o benchmark_parallel

# Compile tests
g++ -std=c++17 -O3 -pthread -I include tests/linearizability_test.cpp -o test_linearizability
```

### Run

```bash
# Scalability benchmark
./benchmark_parallel

# Routing strategies comparison
./benchmark_routing
```

## Key Features

### 1. Tree-of-Trees Architecture
Eliminates structural contention by partitioning keys across independent shards.

### 2. Adaptive Routing
Four strategies available:
- **STATIC_HASH**: Simple modulo routing
- **LOAD_AWARE**: Real-time load balancing
- **VIRTUAL_NODES**: Consistent hashing
- **INTELLIGENT**: Hybrid approach (recommended)

### 3. Linearizability
All operations are linearizable - if `insert(K)` completes, `contains(K)` will find it.

## Academic Paper

The `paper/` directory contains a rigorous academic paper describing:
- Theoretical foundations
- Algorithm analysis
- Experimental evaluation
- Comparison with related work

To compile the paper:
```bash
cd paper
make
```

## Performance Results

| Threads | Speedup | Efficiency |
|---------|---------|------------|
| 1       | 1.00×   | 100%       |
| 2       | 1.98×   | 99%        |
| 4       | 3.91×   | 98%        |
| 8       | 7.78×   | 97%        |

## License

See LICENSE file in parent directory.
