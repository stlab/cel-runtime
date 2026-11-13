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

/// A shared runtime node with allocation-free last-owner retirement.
///
/// Distinct retained handles may release concurrently; only the last release grants
/// exclusive retirement ownership.
struct retirement_node {
    // [knowledge] Live shared nodes never mutate next; only the last-owner queue links
    // dying nodes. Keep the embedded count lock-free and the disposal callback nonthrowing.
    std::atomic<std::size_t> owners{1}; ///< Counts owning handles, not borrowed observers.
    retirement_node* next = nullptr;    ///< Links exclusively owned dying nodes.
    void (*dispose)(retirement_node&, retirement_queue&) noexcept; ///< Detaches edges and deletes.

    /// Installs the concrete node's nonthrowing detach-and-delete operation.
    ///
    /// \pre operation retires runtime-owned child edges into its supplied queue before
    /// deleting the node, without allocating runtime traversal storage. Client destructor
    /// work retains its own costs and behavior.
    explicit retirement_node(void (*operation)(retirement_node&, retirement_queue&) noexcept) :
        dispose(operation) {}
};

/// An exclusive owner of nodes awaiting last-owner retirement.
///
/// Destruction disposes every last-owned descendant; shared children remain alive.
/// Each operation uses its own queue. Runtime traversal allocates nothing and uses
/// constant stack space, including during exception unwinding.
class retirement_queue {
    // [knowledge] Reuse dying nodes' links as the worklist; never allocate a traversal
    // container or recursively destroy runtime-owned executable edges.
    /// Owns nodes awaiting disposal.
    retirement_node* _pending = nullptr;

public:
    /// Creates an empty local retirement operation.
    retirement_queue() noexcept = default;
    /// Disallows duplicated ownership of dying nodes.
    retirement_queue(const retirement_queue&) = delete;
    /// Disallows duplicated assignment.
    retirement_queue& operator=(const retirement_queue&) = delete;
    /// Drains every dying descendant without allocating traversal storage.
    ///
    /// Complexity: O(retired nodes, edges, and instructions) time plus released metadata
    /// and client destructor work; O(1) runtime auxiliary space.
    ~retirement_queue() {
        while (_pending) {
            auto* current = _pending;
            _pending = current->next;
            current->next = nullptr;
            current->dispose(*current, *this);
        }
    }
    /// Releases one reference and enqueues exactly the last owner's node.
    ///
    /// \pre node is null or denotes one retained ownership reference; this queue is
    /// exclusively accessed.
    /// Does not dispose the node until this queue drains. Concurrent releases through
    /// distinct queues preserve shared children until their last ownership reference dies.
    void release(retirement_node* node) noexcept {
        // [knowledge] Identify last ownership at decrement, not with use_count plus reset:
        // another handle may release concurrently between a snapshot and the decrement.
        if (node && node->owners.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            node->next = _pending;
            _pending = node;
        }
    }
};

/// A reference-semantic owning handle to a runtime node.
///
/// \pre T derives from retirement_node.
/// Copies share the node; moves leave the source empty. Distinct handles may retain and
/// release concurrently; mutation of one handle requires exclusive access.
/// Ownership does not synchronize mutable callbacks or guards. Last release preserves
/// shared children and retires runtime-owned descendants without traversal allocation
/// or nesting-dependent stack, including during unwinding.
///
/// Retirement costs O(last-owned nodes, edges, and instructions) time plus released
/// metadata and client destructor work, with O(1) runtime auxiliary space.
// [knowledge] Keep the reference count in the node, with no separate control-block allocation.
template <class T>
class retirement_ptr {
    /// Owns one retained node reference, or none when null.
    retirement_node* _node = nullptr;

public:
    /// Creates an empty handle.
    retirement_ptr() noexcept = default;
    /// Adopts a newly constructed nonconst node with exactly its initial ownership reference.
    ///
    /// \pre node is null or has not been adopted by another handle.
    explicit retirement_ptr(std::remove_const_t<T>* node) noexcept : _node(node) {}
    /// Retains another reference-semantic handle.
    retirement_ptr(const retirement_ptr& other) noexcept : _node(other._node) {
        if (_node) _node->owners.fetch_add(1, std::memory_order_relaxed);
    }
    /// Transfers the ownership reference and empties the source.
    retirement_ptr(retirement_ptr&& other) noexcept : _node(std::exchange(other._node, nullptr)) {}
    /// Releases the reference and iteratively disposes any last-owned descendants.
    ///
    /// Complexity: retirement costs.
    ~retirement_ptr() {
        retirement_queue queue;
        retire(queue);
    }
    /// Replaces prior ownership with a shared reference to other's node.
    ///
    /// Self-copy preserves ownership; the incoming node stays alive while prior ownership retires.
    ///
    /// Complexity: prior ownership retirement costs.
    retirement_ptr& operator=(const retirement_ptr& other) noexcept {
        retirement_ptr next(other);
        swap(next);
        return *this;
    }
    /// Retires prior ownership and transfers other's reference, leaving other empty.
    ///
    /// Self-move leaves ownership unchanged.
    ///
    /// Complexity: prior ownership retirement costs.
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
    ///
    /// Last-owned nodes remain owned by queue until it drains.
    void retire(retirement_queue& queue) noexcept { queue.release(std::exchange(_node, nullptr)); }
    /// Reports whether this handle owns a node.
    explicit operator bool() const noexcept { return _node != nullptr; }
    /// Borrows the retained concrete node.
    ///
    /// \pre This handle owns a node.
    /// The pointer acquires no ownership; a retained handle must keep the node alive
    /// throughout every use.
    T* operator->() const noexcept {
        assert(_node);
        return static_cast<T*>(_node);
    }
};
} // namespace cel::detail
#endif
