/**
 * @file compare_cpp_c.cpp
 * @brief Benchmark to compare C++ vs C implementation performance
 */

#include "../include/parallel_avl.hpp"
#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <random>
#include <iomanip>

using namespace std;
using namespace std::chrono;

// Fast xorshift64 RNG
static inline uint64_t xorshift64(uint64_t* state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

template<typename TreeType>
void worker(TreeType& tree, size_t ops, int key_range, int thread_id) {
    uint64_t rng = 12345ULL + thread_id * 1000ULL;
    
    for (size_t i = 0; i < ops; i++) {
        int64_t key = (int64_t)(xorshift64(&rng) % (uint64_t)key_range);
        uint64_t op = xorshift64(&rng) % 100;
        
        if (op < 70) {  // 70% reads
            tree.contains(key);
        } else if (op < 85) {  // 15% inserts
            tree.insert(key, key);
        } else {  // 15% deletes
            tree.remove(key);
        }
    }
}

double benchmark_cpp_parallel(size_t num_threads, size_t ops_per_thread, int key_range) {
    ParallelAVL<int64_t, int64_t> tree(num_threads, 
        ParallelAVL<int64_t, int64_t>::RouterStrategy::STATIC_HASH);
    
    // Pre-populate
    for (int i = 0; i < 1000; i++) {
        tree.insert(i, i);
    }
    
    auto start = high_resolution_clock::now();
    
    vector<thread> workers;
    for (size_t i = 0; i < num_threads; i++) {
        workers.emplace_back(worker<ParallelAVL<int64_t, int64_t>>, 
                            ref(tree), ops_per_thread, key_range, (int)i);
    }
    
    for (auto& t : workers) {
        t.join();
    }
    
    auto end = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end - start);
    double seconds = max(duration.count() / 1000.0, 0.001);
    
    return (num_threads * ops_per_thread) / seconds;
}

void print_header(const string& title) {
    cout << "\n+------------------------------------------------------------+" << endl;
    cout << "|  " << setw(56) << left << title << "  |" << endl;
    cout << "+------------------------------------------------------------+\n" << endl;
}

int main() {
    print_header("C++ ParallelAVL Benchmark");
    
    const size_t TOTAL_OPS = 1000000;
    const int KEY_RANGE = 100000;
    
    cout << "Total operations: " << TOTAL_OPS << endl;
    cout << "Key range: " << KEY_RANGE << "\n" << endl;
    
    size_t thread_counts[] = {2, 4, 8};
    
    for (size_t threads : thread_counts) {
        size_t ops_per_thread = TOTAL_OPS / threads;
        
        cout << "C++ ParallelAVL (" << threads << " threads, " << threads << " shards):" << endl;
        double throughput = benchmark_cpp_parallel(threads, ops_per_thread, KEY_RANGE);
        cout << "  Throughput: " << fixed << setprecision(0) << throughput << " ops/sec" << endl;
        cout << endl;
    }
    
    print_header("Benchmark Complete");
    
    return 0;
}
