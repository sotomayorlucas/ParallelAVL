#ifndef BASE_TREE_H
#define BASE_TREE_H

#include <cstddef>

// Abstract base interface for tree implementations
// Provides a common API for sequential and parallel trees

template<typename Key, typename Value = Key>
class BaseTree {
public:
    virtual ~BaseTree() = default;

    // Core operations
    virtual void insert(const Key& key, const Value& value = Value()) = 0;
    virtual void remove(const Key& key) = 0;
    virtual bool contains(const Key& key) const = 0;
    virtual Value get(const Key& key) const = 0;
    virtual std::size_t size() const = 0;
};

#endif // BASE_TREE_H
