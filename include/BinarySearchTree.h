#ifndef BINARY_SEARCH_TREE_H
#define BINARY_SEARCH_TREE_H

#include <cstddef>
#include <functional>

// Base Binary Search Tree implementation
// Provides foundation for AVL tree with virtual rebalance hook

template<typename Key, typename Value = Key>
class BinarySearchTree {
public:
    struct Node {
        Key key;
        Value value;
        Node* left = nullptr;
        Node* right = nullptr;
        Node* parent = nullptr;
        int height = 1;  // For AVL use

        Node(const Key& k, const Value& v) : key(k), value(v) {}
    };

protected:
    Node* root_ = nullptr;
    size_t size_ = 0;

    // Virtual rebalance hook for derived classes (AVL, etc.)
    virtual void rebalance(Node* start) { (void)start; }

    Node* findNode(const Key& key) const {
        Node* current = root_;
        while (current) {
            if (key < current->key) {
                current = current->left;
            } else if (key > current->key) {
                current = current->right;
            } else {
                return current;
            }
        }
        return nullptr;
    }

    Node* findMin(Node* node) const {
        while (node && node->left) {
            node = node->left;
        }
        return node;
    }

    Node* findMax(Node* node) const {
        while (node && node->right) {
            node = node->right;
        }
        return node;
    }

    void transplant(Node* u, Node* v) {
        if (!u->parent) {
            root_ = v;
        } else if (u == u->parent->left) {
            u->parent->left = v;
        } else {
            u->parent->right = v;
        }
        if (v) {
            v->parent = u->parent;
        }
    }

    void destroyTree(Node* node) {
        if (node) {
            destroyTree(node->left);
            destroyTree(node->right);
            delete node;
        }
    }

public:
    BinarySearchTree() = default;

    virtual ~BinarySearchTree() {
        destroyTree(root_);
    }

    // Prevent copying (has raw pointers)
    BinarySearchTree(const BinarySearchTree&) = delete;
    BinarySearchTree& operator=(const BinarySearchTree&) = delete;

    // Allow move
    BinarySearchTree(BinarySearchTree&& other) noexcept
        : root_(other.root_), size_(other.size_) {
        other.root_ = nullptr;
        other.size_ = 0;
    }

    BinarySearchTree& operator=(BinarySearchTree&& other) noexcept {
        if (this != &other) {
            destroyTree(root_);
            root_ = other.root_;
            size_ = other.size_;
            other.root_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }

    void insert(const Key& key, const Value& value) {
        Node* parent = nullptr;
        Node* current = root_;

        while (current) {
            parent = current;
            if (key < current->key) {
                current = current->left;
            } else if (key > current->key) {
                current = current->right;
            } else {
                // Key exists, update value
                current->value = value;
                return;
            }
        }

        Node* newNode = new Node(key, value);
        newNode->parent = parent;

        if (!parent) {
            root_ = newNode;
        } else if (key < parent->key) {
            parent->left = newNode;
        } else {
            parent->right = newNode;
        }

        ++size_;
        rebalance(newNode);
    }

    void remove(const Key& key) {
        Node* node = findNode(key);
        if (!node) return;

        Node* rebalanceStart = nullptr;

        if (!node->left) {
            rebalanceStart = node->parent;
            transplant(node, node->right);
        } else if (!node->right) {
            rebalanceStart = node->parent;
            transplant(node, node->left);
        } else {
            Node* successor = findMin(node->right);
            rebalanceStart = (successor->parent == node) ? successor : successor->parent;

            if (successor->parent != node) {
                transplant(successor, successor->right);
                successor->right = node->right;
                successor->right->parent = successor;
            }

            transplant(node, successor);
            successor->left = node->left;
            successor->left->parent = successor;
        }

        delete node;
        --size_;

        if (rebalanceStart) {
            rebalance(rebalanceStart);
        }
    }

    bool contains(const Key& key) const {
        return findNode(key) != nullptr;
    }

    Value get(const Key& key) const {
        Node* node = findNode(key);
        if (node) {
            return node->value;
        }
        return Value{};
    }

    size_t size() const { return size_; }

    bool empty() const { return size_ == 0; }

    Key minKey() const {
        Node* node = findMin(root_);
        return node ? node->key : Key{};
    }

    Key maxKey() const {
        Node* node = findMax(root_);
        return node ? node->key : Key{};
    }
};

#endif // BINARY_SEARCH_TREE_H
