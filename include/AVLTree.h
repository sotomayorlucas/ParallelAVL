#ifndef AVL_TREE_H
#define AVL_TREE_H

#include "BinarySearchTree.h"
#include <algorithm>

// AVL tree implementation derived from BinarySearchTree. The Node used by the
// base class already stores the required links, we only add a height field to
// assist with balancing.

template<typename Key, typename Value = Key>
class AVLTree : public BinarySearchTree<Key, Value> {
    using Base = BinarySearchTree<Key, Value>;

public:
    using Node = typename Base::Node;

private:

    int height(Node* n) const { return n ? n->height : 0; }

    int balanceFactor(Node* n) const { return height(n->right) - height(n->left); }

    void updateHeight(Node* n) {
        if (n) n->height = 1 + std::max(height(n->left), height(n->right));
    }

    Node* rotateLeft(Node* x) {
        Node* y = x->right;
        Node* B = y->left;
        y->left = x;
        x->right = B;
        if (B) B->parent = x;
        y->parent = x->parent;
        if (!x->parent)
            this->root_ = y;
        else if (x->parent->left == x)
            x->parent->left = y;
        else
            x->parent->right = y;
        x->parent = y;
        updateHeight(x);
        updateHeight(y);
        return y;
    }

    Node* rotateRight(Node* x) {
        Node* y = x->left;
        Node* B = y->right;
        y->right = x;
        x->left = B;
        if (B) B->parent = x;
        y->parent = x->parent;
        if (!x->parent)
            this->root_ = y;
        else if (x->parent->left == x)
            x->parent->left = y;
        else
            x->parent->right = y;
        x->parent = y;
        updateHeight(x);
        updateHeight(y);
        return y;
    }

    Node* rebalanceNode(Node* n) {
        updateHeight(n);
        int bf = balanceFactor(n);
        if (bf == 2) {
            if (balanceFactor(n->right) < 0)
                rotateRight(n->right);
            return rotateLeft(n);
        } else if (bf == -2) {
            if (balanceFactor(n->left) > 0)
                rotateLeft(n->left);
            return rotateRight(n);
        }
        return n;
    }

protected:
    void rebalance(Node* start) override {
        Node* n = start;
        while (n) {
            n = rebalanceNode(n);
            n = n->parent;
        }
    }

public:
    AVLTree() : Base() {}

    // Allow access to root for parallel tree extraction
    Node* getRoot() const { return this->root_; }
};

#endif // AVL_TREE_H
