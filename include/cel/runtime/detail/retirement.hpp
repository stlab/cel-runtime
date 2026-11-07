// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_DETAIL_RETIREMENT_HPP
#define CEL_RUNTIME_DETAIL_RETIREMENT_HPP
#include <atomic>
#include <cassert>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace cel::detail {
static_assert(std::atomic<std::size_t>::is_always_lock_free,
              "runtime ownership requires lock-free reference counts");
class retirement_queue;

/// Retains an embedded ownership count and a link used only after the last release.
/// Live shared nodes never mutate their retirement link; the retiring queue owns dying nodes.
struct retirement_node {
    std::atomic<std::size_t> owners{1}; ///< Counts owning handles, not borrowed observers.
    retirement_node* next = nullptr;    ///< Links exclusively owned dying nodes.
    void (*dispose)(retirement_node&, retirement_queue&) noexcept; ///< Detaches edges and deletes.

    /// Installs the concrete node's nonthrowing detach-and-delete operation.
    explicit retirement_node(void (*operation)(retirement_node&, retirement_queue&) noexcept) :
        dispose(operation) {}
};

/// Owns dying executable nodes in an intrusive LIFO worklist.
/// Detach operations retire runtime-owned child edges into this same queue before deletion.
/// No queue is shared between operations; traversal allocates nothing and uses constant stack.
class retirement_queue {
    retirement_node* _pending = nullptr;

public:
    /// Creates an empty local retirement operation.
    retirement_queue() noexcept = default;
    /// Disallows duplicated ownership of dying nodes.
    retirement_queue(const retirement_queue&) = delete;
    /// Disallows duplicated assignment.
    retirement_queue& operator=(const retirement_queue&) = delete;
    /// Drains every dying descendant without allocating traversal storage.
    /// Complexity: O(retired nodes/edges + released metadata + client destructor work).
    ~retirement_queue() {
        while (_pending) {
            auto* current = _pending;
            _pending = current->next;
            current->next = nullptr;
            current->dispose(*current, *this);
        }
    }
    /// Releases one reference and enqueues exactly the last owner's node.
    /// Precondition: node is null or denotes one retained ownership reference.
    void release(retirement_node* node) noexcept {
        // [knowledge] Identify last ownership at decrement, not with use_count plus reset:
        // another handle may release concurrently between a snapshot and the decrement.
        if (node && node->owners.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            node->next = _pending;
            _pending = node;
        }
    }
};

/// Shares a runtime-owned node while routing last release through a local retirement queue.
/// T derives from retirement_node; ownership does not synchronize mutable callbacks or guards.
/// Copies share the node; moves leave the source empty. No separate control block is allocated.
template <class T>
class retirement_ptr {
    retirement_node* _node = nullptr;

public:
    /// Creates an empty handle.
    retirement_ptr() noexcept = default;
    /// Adopts a newly constructed nonconst node with exactly its initial ownership reference.
    /// Precondition: node is null or has not been adopted by another handle.
    explicit retirement_ptr(std::remove_const_t<T>* node) noexcept : _node(node) {}
    /// Retains another reference-semantic handle.
    retirement_ptr(const retirement_ptr& other) noexcept : _node(other._node) {
        if (_node) _node->owners.fetch_add(1, std::memory_order_relaxed);
    }
    /// Transfers the ownership reference and empties the source.
    retirement_ptr(retirement_ptr&& other) noexcept : _node(std::exchange(other._node, nullptr)) {}
    /// Releases the reference and iteratively disposes any last-owned descendants.
    /// Complexity: O(last-owned nodes/edges + released metadata + client destructor work).
    ~retirement_ptr() {
        retirement_queue queue;
        retire(queue);
    }
    /// Shares another handle, retaining it before releasing prior ownership; self-copy is safe.
    /// Complexity: O(prior last-owned nodes/edges + released metadata + client destructor work).
    retirement_ptr& operator=(const retirement_ptr& other) noexcept {
        retirement_ptr next(other);
        swap(next);
        return *this;
    }
    /// Transfers another handle and releases prior ownership; self-move is unchanged.
    /// Complexity: O(prior last-owned nodes/edges + released metadata + client destructor work).
    retirement_ptr& operator=(retirement_ptr&& other) noexcept {
        if (this != &other) {
            retirement_ptr next(std::move(other));
            swap(next);
        }
        return *this;
    }
    /// Exchanges ownership references without changing their counts.
    void swap(retirement_ptr& other) noexcept { std::swap(_node, other._node); }
    /// Releases this edge into an existing operation and empties the handle.
    void retire(retirement_queue& queue) noexcept { queue.release(std::exchange(_node, nullptr)); }
    /// Reports whether this handle owns a node.
    explicit operator bool() const noexcept { return _node != nullptr; }
    /// Borrows the retained concrete node.
    /// Precondition: this handle owns a node.
    T* operator->() const noexcept {
        assert(_node);
        return static_cast<T*>(_node);
    }
};
} // namespace cel::detail
#endif
