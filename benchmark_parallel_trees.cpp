#include "include/AVLTree.h"
#include "include/AVLTreeConcurrent.h"
#include "include/AVLTreeParallel.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <random>
#include <iomanip>
#include <atomic>

using namespace std;
using namespace std::chrono;

template<typename TreeType>
void worker(TreeType& tree, size_t ops, int key_range) {
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<> key_dis(0, key_range);
    uniform_int_distribution<> op_dis(0, 99);

    for (size_t i = 0; i < ops; ++i) {
        int key = key_dis(gen);
        int op = op_dis(gen);

        if (op < 70) {  // 70% reads
            tree.contains(key);
        } else if (op < 85) {  // 15% inserts
            tree.insert(key, key);
        } else {  // 15% deletes
            tree.remove(key);
        }
    }
}

void printHeader(const string& title) {
    cout << "\n╔";
    for (size_t i = 0; i < 68; ++i) cout << "═";
    cout << "╗" << endl;
    cout << "║  " << setw(64) << left << title << "  ║" << endl;
    cout << "╚";
    for (size_t i = 0; i < 68; ++i) cout << "═";
    cout << "╝\n" << endl;
}

// Benchmark single-threaded (baseline)
double benchmarkSingleThread(size_t total_ops, int key_range) {
    AVLTree<int> tree;

    // Pre-populate
    for (int i = 0; i < 1000; ++i) {
        tree.insert(i, i);
    }

    auto start = high_resolution_clock::now();
    worker(tree, total_ops, key_range);
    auto end = high_resolution_clock::now();

    auto duration = duration_cast<milliseconds>(end - start);
    double seconds = max(duration.count() / 1000.0, 0.001);
    return total_ops / seconds;
}

// Benchmark global lock (serializado)
double benchmarkGlobalLock(size_t threads, size_t ops_per_thread, int key_range) {
    AVLTreeConcurrent<int> tree;

    // Pre-populate
    for (int i = 0; i < 1000; ++i) {
        tree.insert(i, i);
    }

    auto start = high_resolution_clock::now();

    vector<thread> workers;
    for (size_t i = 0; i < threads; ++i) {
        workers.emplace_back(worker<AVLTreeConcurrent<int>>, ref(tree), ops_per_thread, key_range);
    }

    for (auto& t : workers) {
        t.join();
    }

    auto end = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end - start);
    double seconds = max(duration.count() / 1000.0, 0.001);
    return (threads * ops_per_thread) / seconds;
}

// Benchmark parallel trees (verdadero paralelismo!)
double benchmarkParallelTrees(size_t threads, size_t ops_per_thread, int key_range) {
    // Crear árbol paralelo con N shards = N threads
    AVLTreeParallel<int> tree(threads, AVLTreeParallel<int>::RoutingStrategy::HASH);

    // Pre-populate
    for (int i = 0; i < 1000; ++i) {
        tree.insert(i, i);
    }

    auto start = high_resolution_clock::now();

    vector<thread> workers;
    for (size_t i = 0; i < threads; ++i) {
        workers.emplace_back(worker<AVLTreeParallel<int>>, ref(tree), ops_per_thread, key_range);
    }

    for (auto& t : workers) {
        t.join();
    }

    auto end = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end - start);
    double seconds = max(duration.count() / 1000.0, 0.001);

    return (threads * ops_per_thread) / seconds;
}

void runScalabilityTest(size_t num_threads) {
    printHeader(to_string(num_threads) + " Threads - Scalability Test");

    const size_t OPS_PER_THREAD = 10000;
    const int KEY_RANGE = 10000;

    cout << "Total operations: " << (num_threads * OPS_PER_THREAD) << endl;
    cout << "Operations per thread: " << OPS_PER_THREAD << "\n" << endl;

    // Baseline single-threaded
    cout << "Baseline (single-threaded):" << endl;
    double baseline = benchmarkSingleThread(num_threads * OPS_PER_THREAD, KEY_RANGE);
    cout << "  Throughput: " << fixed << setprecision(0) << baseline << " ops/sec" << endl;

    cout << "\nGlobal Lock (serialized):" << endl;
    double global_lock = benchmarkGlobalLock(num_threads, OPS_PER_THREAD, KEY_RANGE);
    cout << "  Throughput: " << fixed << setprecision(0) << global_lock << " ops/sec" << endl;

    cout << "\nParallel Trees (N shards = N threads):" << endl;
    double parallel = benchmarkParallelTrees(num_threads, OPS_PER_THREAD, KEY_RANGE);
    cout << "  Throughput: " << fixed << setprecision(0) << parallel << " ops/sec" << endl;

    // Analysis
    cout << "\n📊 SPEEDUP vs Single-threaded Baseline:" << endl;
    cout << "  Global Lock:    " << fixed << setprecision(2) << (global_lock / baseline) << "x";
    if (global_lock > baseline) cout << " ✅";
    else cout << " ❌";
    cout << endl;

    cout << "  Parallel Trees: " << fixed << setprecision(2) << (parallel / baseline) << "x";
    if (parallel > baseline) cout << " ✅";
    else cout << " ❌";
    cout << endl;

    cout << "\n📊 SPEEDUP Parallel vs Global Lock:" << endl;
    cout << "  " << fixed << setprecision(2) << (parallel / global_lock) << "x";
    if (parallel > global_lock * 1.2) {
        cout << " ✅ SIGNIFICATIVO!" << endl;
    } else if (parallel > global_lock) {
        cout << " ✅ Mejor" << endl;
    } else {
        cout << " ❌ No mejor" << endl;
    }

    cout << "\n📈 SCALABILITY:" << endl;
    cout << "  Ideal speedup:   " << num_threads << "x" << endl;
    cout << "  Parallel actual: " << fixed << setprecision(2)
         << (parallel / baseline) << "x" << endl;
    cout << "  Efficiency:      " << fixed << setprecision(1)
         << ((parallel / baseline) / num_threads * 100) << "%" << endl;

    if ((parallel / baseline) / num_threads > 0.7) {
        cout << "\n  ✅ Excelente escalabilidad!" << endl;
    } else if ((parallel / baseline) / num_threads > 0.4) {
        cout << "\n  ⚠️  Escalabilidad moderada" << endl;
    } else {
        cout << "\n  ❌ Pobre escalabilidad" << endl;
    }
}

void demonstrateArchitecture() {
    printHeader("Parallel Trees Architecture Demo");

    size_t num_cores = thread::hardware_concurrency();
    cout << "Hardware concurrency: " << num_cores << " cores\n" << endl;

    // Crear árbol paralelo
    AVLTreeParallel<int> tree(num_cores, AVLTreeParallel<int>::RoutingStrategy::HASH);

    // Insertar datos
    cout << "Inserting 10,000 elements..." << endl;
    for (int i = 0; i < 10000; ++i) {
        tree.insert(i, i * 2);
    }

    // Mostrar distribución
    tree.printDistribution();

    auto info = tree.getArchitectureInfo();

    cout << "\n💡 Key Insights:" << endl;
    if (info.load_balance_score > 0.9) {
        cout << "  ✅ Excelente balanceo entre shards" << endl;
    } else if (info.load_balance_score > 0.7) {
        cout << "  ⚠️  Balanceo aceptable, podría mejorar" << endl;
    } else {
        cout << "  ❌ Desbalanceado - considerar rebalanceo" << endl;
    }

    cout << "\n  Con " << num_cores << " cores:" << endl;
    cout << "    → " << num_cores << " árboles independientes" << endl;
    cout << "    → " << num_cores << " operaciones simultáneas posibles" << endl;
    cout << "    → Speedup teórico: " << num_cores << "x" << endl;
}

int main() {
    printHeader("Parallel AVL Trees: N-Trees Architecture");

    cout << "Esta implementación usa N árboles independientes:\n";
    cout << "  • Un árbol por core/thread\n";
    cout << "  • Cada árbol tiene su propio lock (simple, eficiente)\n";
    cout << "  • Routing determina qué árbol usar para cada key\n";
    cout << "  • Operaciones en diferentes árboles = paralelismo REAL\n" << endl;

    cout << "Comparación:\n";
    cout << "  Global Lock:    1 árbol, 1 lock  → Serializado\n";
    cout << "  Granular Lock:  1 árbol, N locks → Overhead alto\n";
    cout << "  Parallel Trees: N árboles, N locks → Verdadero paralelismo ✅\n" << endl;

    // Demo de arquitectura
    demonstrateArchitecture();

    // Scalability tests
    cout << "\n";
    for (size_t threads : {2, 4, 8}) {
        runScalabilityTest(threads);
        cout << "\n";
    }

    printHeader("Conclusión");

    cout << "Resultados esperados:\n";
    cout << "  • Global Lock: Speedup < 1x (lock contention)\n";
    cout << "  • Parallel Trees: Speedup ≈ N/2 to 3N/4 (verdadero paralelismo!)\n" << endl;

    cout << "Por qué funciona:\n";
    cout << "  1. Sin contención entre árboles diferentes\n";
    cout << "  2. Global lock en cada árbol (simple, eficiente)\n";
    cout << "  3. Hash routing distribuye carga uniformemente\n";
    cout << "  4. Cada thread puede trabajar en su propio árbol\n" << endl;

    cout << "Trade-offs:\n";
    cout << "  ✅ Excelente paralelismo\n";
    cout << "  ✅ Escalabilidad lineal (teórica)\n";
    cout << "  ❌ Range queries más complejas\n";
    cout << "  ❌ Puede requerir rebalanceo\n" << endl;

    return 0;
}
