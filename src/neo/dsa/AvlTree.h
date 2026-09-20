#pragma once

#include "neo/dsa/Instrumentation.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace neo {
namespace dsa {

// AVL tree over (key, payload) pairs, stored in a vector node pool.
//
// WHAT IT IS FOR
//   The ordered index the query engine ranges over when the range itself is the
//   selective part of the query: approaches by date ("everything between 2030
//   and 2040"), and the same shape for distance or velocity. It answers
//   range(lo, hi) in O(log n + m) and, unlike a sorted array, stays correct
//   while rows are inserted or erased.
//
// COMPLEXITY (n nodes)
//   find / insert / erase    O(log n) guaranteed; AVL keeps the height below
//                            1.44 * log2(n + 2), so 42,819 approaches are at
//                            most ~22 levels deep
//   range(lo, hi)            O(log n + m) for m keys reported
//   inOrder                  O(n)
//   space                    n * sizeof(Node) in one vector, plus a free list
//
// WHY AVL AND NOT AN INTERVAL TREE
//   A close approach is an instant, not an interval: it has a date, not a start
//   and an end. A date window is therefore two boundary searches plus an
//   in-order walk, which a plain ordered tree does. An interval tree only earns
//   its extra machinery (a subtree-maximum per node, maintained through every
//   rotation) when stored items have duration and can overlap. Choosing it here
//   would be complexity with no query to justify it.
//
// WHY AVL AND NOT A RED-BLACK TREE
//   AVL is more strictly balanced (height ~1.44 log n against ~2 log n), which
//   favours the read-heavy workload here: the index is built once per dataset
//   and then queried repeatedly. Its invariant is also easy to state and check
//   exactly, which is what the invariant checker below does.
//
// WHY A NODE POOL
//   Nodes live in one std::vector and link to each other by std::int32_t index,
//   so building the index is a handful of allocations instead of one per node,
//   the nodes are contiguous in memory, and an erased node's slot is reused
//   through a free list. Indices also make the structure trivially copyable and
//   serialisable, which pointers would not be.
//
// DUPLICATE KEYS
//   Many approaches share a date, and many objects share a diameter. The tree
//   therefore orders by the composite (key, payload): the payload is the record
//   or approach index, so every entry is unique while the primary ordering
//   stays by key. range(lo, hi) still returns every entry whose key is in
//   [lo, hi], in (key, payload) order.

template <class Key, class Counters = NullCounters>
class AvlTree : private Counters {
public:
    using Payload = std::uint32_t;
    static constexpr std::int32_t kNil = -1;

    struct Entry {
        Key     key;
        Payload payload = 0;
    };

    AvlTree() = default;

    void reserve(std::size_t n) { nodes_.reserve(n); }

    // Inserts (key, payload). Returns false when that exact pair is already
    // present (the tree is a set of pairs, so the same approach cannot be
    // indexed twice).
    bool insert(const Key& key, Payload payload) {
        bool inserted = false;
        root_ = insertAt(root_, key, payload, inserted);
        if (inserted) {
            ++size_;
        }
        return inserted;
    }

    bool contains(const Key& key, Payload payload) const { return findNode(key, payload) != kNil; }

    bool erase(const Key& key, Payload payload) {
        bool removed = false;
        root_ = eraseAt(root_, key, payload, removed);
        if (removed) {
            --size_;
        }
        return removed;
    }

    void clear() {
        nodes_.clear();
        root_ = kNil;
        freeList_ = kNil;
        size_ = 0;
    }

    std::size_t size() const { return size_; }
    bool        empty() const { return size_ == 0; }
    int         height() const { return heightOf(root_); }
    std::size_t memoryBytes() const { return nodes_.capacity() * sizeof(Node); }

    // fn(const Key&, Payload) over every entry, ascending.
    template <class Fn>
    void inOrder(Fn&& fn) const {
        // Iterative with an explicit stack: the depth is bounded by the AVL
        // height, so this cannot overflow the call stack on a big index.
        std::vector<std::int32_t> stack;
        stack.reserve(static_cast<std::size_t>(height()) + 1);
        std::int32_t node = root_;
        while (node != kNil || !stack.empty()) {
            while (node != kNil) {
                stack.push_back(node);
                node = nodes_[static_cast<std::size_t>(node)].left;
            }
            node = stack.back();
            stack.pop_back();
            const Node& current = nodes_[static_cast<std::size_t>(node)];
            fn(current.key, current.payload);
            node = current.right;
        }
    }

    // fn(const Key&, Payload) for every entry whose key is in [lo, hi].
    // Descends to the lower bound, then walks in order until past `hi`:
    // O(log n) to arrive plus O(1) per reported entry.
    template <class Fn>
    std::size_t range(const Key& lo, const Key& hi, Fn&& fn) const {
        std::size_t visited = 0;
        if (root_ == kNil || hi < lo) {
            return 0;
        }
        std::vector<std::int32_t> stack;
        stack.reserve(static_cast<std::size_t>(height()) + 1);
        std::int32_t node = root_;
        // Walk down to the first key >= lo, remembering the nodes we may still
        // have to report (those whose key is not below lo).
        while (node != kNil) {
            const Node& current = nodes_[static_cast<std::size_t>(node)];
            this->comparison();
            if (current.key < lo) {
                node = current.right; // everything left of here is too small
            } else {
                stack.push_back(node);
                node = current.left;
            }
        }
        while (!stack.empty()) {
            const std::int32_t index = stack.back();
            stack.pop_back();
            const Node& current = nodes_[static_cast<std::size_t>(index)];
            this->comparison();
            if (hi < current.key) {
                break; // past the window: everything further right is greater
            }
            fn(current.key, current.payload);
            ++visited;
            std::int32_t right = current.right;
            while (right != kNil) {
                stack.push_back(right);
                right = nodes_[static_cast<std::size_t>(right)].left;
            }
        }
        return visited;
    }

    std::vector<Entry> toVector() const {
        std::vector<Entry> out;
        out.reserve(size_);
        inOrder([&out](const Key& key, Payload payload) { out.push_back(Entry{key, payload}); });
        return out;
    }

    // Full structural check, used by the fuzz test:
    //   * in-order traversal is strictly increasing in (key, payload)
    //   * every node's stored height equals 1 + max(child heights)
    //   * every balance factor is in [-1, 1]
    //   * the node count matches size()
    bool checkInvariants(std::string& error) const {
        std::size_t counted = 0;
        const bool structural = checkNode(root_, counted, error);
        if (!structural) {
            return false;
        }
        if (counted != size_) {
            error = "size() says " + std::to_string(size_) + " but the tree holds " + std::to_string(counted);
            return false;
        }
        bool first = true;
        Key lastKey = Key();
        Payload lastPayload = 0;
        bool ordered = true;
        inOrder([&](const Key& key, Payload payload) {
            if (!first && !(lastKey < key || (!(key < lastKey) && lastPayload < payload))) {
                ordered = false;
            }
            lastKey = key;
            lastPayload = payload;
            first = false;
        });
        if (!ordered) {
            error = "in-order traversal is not strictly increasing in (key, payload)";
            return false;
        }
        return true;
    }

    const Counters& counters() const { return *this; }
    Counters& counters() { return *this; }

private:
    struct Node {
        Key          key;
        Payload      payload = 0;
        std::int32_t left = kNil;
        std::int32_t right = kNil;
        std::int32_t height = 1;
    };

    // (key, payload) ordering, so duplicate keys coexist.
    bool less(const Key& aKey, Payload aPayload, const Key& bKey, Payload bPayload) const {
        this->comparison();
        if (aKey < bKey) {
            return true;
        }
        if (bKey < aKey) {
            return false;
        }
        return aPayload < bPayload;
    }

    int heightOf(std::int32_t node) const {
        return node == kNil ? 0 : nodes_[static_cast<std::size_t>(node)].height;
    }

    int balanceOf(std::int32_t node) const {
        if (node == kNil) {
            return 0;
        }
        const Node& current = nodes_[static_cast<std::size_t>(node)];
        return heightOf(current.left) - heightOf(current.right);
    }

    void updateHeight(std::int32_t node) {
        Node& current = nodes_[static_cast<std::size_t>(node)];
        current.height = 1 + std::max(heightOf(current.left), heightOf(current.right));
    }

    std::int32_t allocate(const Key& key, Payload payload) {
        if (freeList_ != kNil) {
            const std::int32_t index = freeList_;
            freeList_ = nodes_[static_cast<std::size_t>(index)].left; // the free list threads through `left`
            Node& node = nodes_[static_cast<std::size_t>(index)];
            node = Node();
            node.key = key;
            node.payload = payload;
            return index;
        }
        Node node;
        node.key = key;
        node.payload = payload;
        nodes_.push_back(std::move(node));
        return static_cast<std::int32_t>(nodes_.size() - 1);
    }

    void release(std::int32_t index) {
        Node& node = nodes_[static_cast<std::size_t>(index)];
        node.left = freeList_;
        node.right = kNil;
        node.height = 0;
        freeList_ = index;
    }

    //       y                 x
    //      / .              . .
    //     x   C    ->      A   y
    //    / .                  / .
    //   A   B                B   C
    // (dots stand for the missing branch strokes: a trailing backslash would
    //  splice the next line into this comment)
    std::int32_t rotateRight(std::int32_t y) {
        this->rotation();
        const std::int32_t x = nodes_[static_cast<std::size_t>(y)].left;
        const std::int32_t b = nodes_[static_cast<std::size_t>(x)].right;
        nodes_[static_cast<std::size_t>(x)].right = y;
        nodes_[static_cast<std::size_t>(y)].left = b;
        updateHeight(y);
        updateHeight(x);
        return x;
    }

    //     x                   y
    //    / .                 . .
    //   A   y      ->       x   C
    //      / .             / .
    //     B   C           A   B
    std::int32_t rotateLeft(std::int32_t x) {
        this->rotation();
        const std::int32_t y = nodes_[static_cast<std::size_t>(x)].right;
        const std::int32_t b = nodes_[static_cast<std::size_t>(y)].left;
        nodes_[static_cast<std::size_t>(y)].left = x;
        nodes_[static_cast<std::size_t>(x)].right = b;
        updateHeight(x);
        updateHeight(y);
        return y;
    }

    // The four cases: LL and RR need one rotation, LR and RL need two.
    std::int32_t rebalance(std::int32_t node) {
        updateHeight(node);
        const int balance = balanceOf(node);
        if (balance > 1) {
            if (balanceOf(nodes_[static_cast<std::size_t>(node)].left) < 0) {
                nodes_[static_cast<std::size_t>(node)].left =
                    rotateLeft(nodes_[static_cast<std::size_t>(node)].left); // LR
            }
            return rotateRight(node); // LL
        }
        if (balance < -1) {
            if (balanceOf(nodes_[static_cast<std::size_t>(node)].right) > 0) {
                nodes_[static_cast<std::size_t>(node)].right =
                    rotateRight(nodes_[static_cast<std::size_t>(node)].right); // RL
            }
            return rotateLeft(node); // RR
        }
        return node;
    }

    std::int32_t insertAt(std::int32_t node, const Key& key, Payload payload, bool& inserted) {
        if (node == kNil) {
            inserted = true;
            return allocate(key, payload);
        }
        const Key nodeKey = nodes_[static_cast<std::size_t>(node)].key;
        const Payload nodePayload = nodes_[static_cast<std::size_t>(node)].payload;
        if (less(key, payload, nodeKey, nodePayload)) {
            const std::int32_t child = insertAt(nodes_[static_cast<std::size_t>(node)].left, key, payload, inserted);
            nodes_[static_cast<std::size_t>(node)].left = child;
        } else if (less(nodeKey, nodePayload, key, payload)) {
            const std::int32_t child = insertAt(nodes_[static_cast<std::size_t>(node)].right, key, payload, inserted);
            nodes_[static_cast<std::size_t>(node)].right = child;
        } else {
            return node; // the exact pair is already present
        }
        return rebalance(node);
    }

    std::int32_t minimumOf(std::int32_t node) const {
        while (nodes_[static_cast<std::size_t>(node)].left != kNil) {
            node = nodes_[static_cast<std::size_t>(node)].left;
        }
        return node;
    }

    std::int32_t eraseAt(std::int32_t node, const Key& key, Payload payload, bool& removed) {
        if (node == kNil) {
            return kNil;
        }
        const Key nodeKey = nodes_[static_cast<std::size_t>(node)].key;
        const Payload nodePayload = nodes_[static_cast<std::size_t>(node)].payload;
        if (less(key, payload, nodeKey, nodePayload)) {
            nodes_[static_cast<std::size_t>(node)].left =
                eraseAt(nodes_[static_cast<std::size_t>(node)].left, key, payload, removed);
        } else if (less(nodeKey, nodePayload, key, payload)) {
            nodes_[static_cast<std::size_t>(node)].right =
                eraseAt(nodes_[static_cast<std::size_t>(node)].right, key, payload, removed);
        } else {
            removed = true;
            const std::int32_t left = nodes_[static_cast<std::size_t>(node)].left;
            const std::int32_t right = nodes_[static_cast<std::size_t>(node)].right;
            if (left == kNil || right == kNil) {
                const std::int32_t survivor = left != kNil ? left : right;
                release(node);
                return survivor;
            }
            // Two children: copy the in-order successor's contents here and
            // erase the successor instead (it has at most one child).
            const std::int32_t successor = minimumOf(right);
            nodes_[static_cast<std::size_t>(node)].key = nodes_[static_cast<std::size_t>(successor)].key;
            nodes_[static_cast<std::size_t>(node)].payload = nodes_[static_cast<std::size_t>(successor)].payload;
            bool removedSuccessor = false;
            nodes_[static_cast<std::size_t>(node)].right =
                eraseAt(right, nodes_[static_cast<std::size_t>(successor)].key,
                        nodes_[static_cast<std::size_t>(successor)].payload, removedSuccessor);
        }
        return rebalance(node);
    }

    std::int32_t findNode(const Key& key, Payload payload) const {
        std::int32_t node = root_;
        while (node != kNil) {
            const Node& current = nodes_[static_cast<std::size_t>(node)];
            if (less(key, payload, current.key, current.payload)) {
                node = current.left;
            } else if (less(current.key, current.payload, key, payload)) {
                node = current.right;
            } else {
                return node;
            }
        }
        return kNil;
    }

    bool checkNode(std::int32_t node, std::size_t& counted, std::string& error) const {
        if (node == kNil) {
            return true;
        }
        const Node& current = nodes_[static_cast<std::size_t>(node)];
        ++counted;
        if (!checkNode(current.left, counted, error) || !checkNode(current.right, counted, error)) {
            return false;
        }
        const int expected = 1 + std::max(heightOf(current.left), heightOf(current.right));
        if (current.height != expected) {
            error = "node " + std::to_string(node) + " stores height " + std::to_string(current.height) +
                    " but its children imply " + std::to_string(expected);
            return false;
        }
        const int balance = heightOf(current.left) - heightOf(current.right);
        if (balance < -1 || balance > 1) {
            error = "node " + std::to_string(node) + " has balance factor " + std::to_string(balance);
            return false;
        }
        return true;
    }

    std::vector<Node> nodes_;
    std::int32_t      root_ = kNil;
    std::int32_t      freeList_ = kNil;
    std::size_t       size_ = 0;
};

} // namespace dsa
} // namespace neo
