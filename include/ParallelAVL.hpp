#ifndef PARALLEL_AVL_HPP
#define PARALLEL_AVL_HPP

// =============================================================================
// ParallelAVL - Alias unificado para árboles paralelos
// =============================================================================
//
// Este header permite migrar gradualmente de AVLTreeParallel a DynamicShardedTree.
//
// Uso:
//   #include "ParallelAVL.hpp"
//   ParallelAVL<int, int> tree;
//
// Compilación:
//   g++ -DUSE_DYNAMIC_SHARDS ...  → Usa DynamicShardedTree (recomendado)
//   g++ ...                        → Usa AVLTreeParallel (legacy)
//
// =============================================================================

#ifdef USE_DYNAMIC_SHARDS
    #include "DynamicShardedTree.hpp"
    
    template<typename Key, typename Value = Key>
    using ParallelAVL = DynamicShardedTree<Key, Value>;
    
    // Config alias
    template<typename Key, typename Value = Key>
    using ParallelAVLConfig = typename DynamicShardedTree<Key, Value>::Config;
    
#else
    #include "AVLTreeParallel.h"
    
    template<typename Key, typename Value = Key>
    using ParallelAVL = AVLTreeParallel<Key, Value>;
    
    // Dummy config para compatibilidad
    template<typename Key, typename Value = Key>
    struct ParallelAVLConfig {
        size_t initial_shards = 4;
        // Los demás campos no aplican para AVLTreeParallel
    };
    
#endif

// =============================================================================
// Helper: Crear árbol con configuración
// =============================================================================

template<typename Key, typename Value = Key>
ParallelAVL<Key, Value> make_parallel_avl(size_t num_shards = 4) {
#ifdef USE_DYNAMIC_SHARDS
    typename DynamicShardedTree<Key, Value>::Config config;
    config.initial_shards = num_shards;
    return DynamicShardedTree<Key, Value>(config);
#else
    return AVLTreeParallel<Key, Value>(num_shards);
#endif
}

#endif // PARALLEL_AVL_HPP
