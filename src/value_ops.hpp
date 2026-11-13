// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_VALUE_OPS_HPP
#define CEL_RUNTIME_VALUE_OPS_HPP

#include <cel/runtime/detail/retirement.hpp>
#include <cel/runtime/value.hpp>

namespace cel::detail {
struct array_owner;
/// Exclusive ownership of a partially initialized canonical packed array.
///
/// Elements initialize in reverse source order; destruction retires exactly the completed
/// suffix. Transfers use the canonical byte-transfer ownership contract, not native moves.
class array_builder {
    // [knowledge] _suffix counts complete trailing elements; the array owner's prefix
    // remains uninitialized until release_to publishes the complete array.
    std::unique_ptr<array_owner> _owner;
    std::size_t _suffix = 0;

public:
    /// Allocates all packed element storage before ownership transfer.
    ///
    /// Throws std::overflow_error for count times canonical stride overflow.
    /// Complexity: O(1 + count times element byte extent) time and packed result storage;
    /// zero-byte elements incur no per-element initialization.
    array_builder(const value_type& element, std::size_t count);
    /// Retires the initialized suffix with existing canonical ownership operations.
    ///
    /// Never allocates runtime traversal storage or recurses with ownership nesting.
    /// Complexity: O(F + N + L + C + D) time, with costs as in value, including the
    /// suffix's element count in F; O(1) runtime auxiliary space.
    ~array_builder();
    /// Disallows duplicated array ownership.
    array_builder(const array_builder&) = delete;
    /// Disallows duplicated assignment.
    array_builder& operator=(const array_builder&) = delete;
    /// Borrows the next uninitialized reverse-source element destination.
    ///
    /// \pre At least one uninitialized element remains.
    /// The view expires on release or destruction; initialized() changes which element is next.
    std::byte* destination() noexcept;
    /// Records complete initialization of the next reverse-source element.
    ///
    /// \pre destination() now contains exactly one complete valid owned element.
    void initialized() noexcept;
    /// Transfers a fully initialized canonical array carrier into external storage.
    ///
    /// \pre All elements are initialized; destination is aligned, disjoint, uninitialized
    /// array-carrier storage.
    /// Retires builder ownership without cloning or destroying the transferred elements.
    void release_to(std::byte* destination) noexcept;
};

/// Clones one initialized raw value into uninitialized canonical storage.
///
/// \pre destination is an uninitialized, suitably aligned writable extent of type.
/// \pre Except for the promised deferred-callable rejection, source is a disjoint readable
/// initialized canonical extent with valid owned descendants, unchanged throughout cloning.
/// Copies mutable aggregates independently; explicit callable code retains reference semantics.
///
/// Throws std::invalid_argument if callable ownership has not been installed.
/// A deferred callable is rejected before source bytes are read.
/// TODO(verify): This established before-read guarantee lacks evidence for callables
/// nested inside arrays; the broader rejection case needs resolution.
/// Failure destroys every constructed child; destination remains uninitialized.
///
/// Complexity: O(B + N + L + C + D) time and new owned result storage, with costs
/// as in value; O(1) runtime auxiliary space excluding opaque operations.
/// Canonical traversal and cleanup use no nesting-dependent call space.
void clone_payload(const value_type& type, const std::byte* source, std::byte* destination);

/// Destroys an initialized raw value, retiring ownership without releasing its outer bytes.
///
/// \pre data contains exactly one valid owned representation of type and is exclusively
/// accessible for retirement.
/// Never allocates or throws. Bytewise transfer must retire the source without this call.
/// Runtime retirement uses no nesting-dependent call space; client operations retain their costs.
/// Complexity: O(N + L + C + D) time and O(1) runtime auxiliary space, with costs as
/// in value. Scalar arrays require no per-element retirement.
void destroy_payload(const value_type& type, std::byte* data) noexcept;
/// Retires a complete payload into an enclosing runtime executable retirement operation.
///
/// \pre data contains exactly one valid owned representation of type and is exclusively
/// accessible for retirement; queue remains alive until enqueued owners retire.
/// Retires array storage now and schedules installed executable owners with queue.
/// Never allocates runtime traversal storage or recurses with ownership nesting.
/// Complexity: O(N + L + C + D) time and O(1) runtime auxiliary space, with costs as in value.
void destroy_payload(const value_type& type, std::byte* data, retirement_queue& queue) noexcept;

/// Trusted access between public owners and canonical raw representations.
///
/// Does not permit byte-relocation of the public wrapper itself.
struct value_access {
    /// Retires one complete public owner's managed children into the enclosing operation.
    ///
    /// \pre owner is exclusively accessible and queue remains alive until enqueued owners retire.
    /// Leaves owner as source unit and releases its outer bytes and metadata reference.
    /// Never allocates runtime traversal storage or recurses with ownership nesting.
    /// Complexity: O(N + L + C + D) time and O(1) runtime auxiliary space, with costs as in value.
    static void retire(value& owner, retirement_queue& queue) noexcept;
    /// Transfers canonical ownership into a caller-provided uninitialized extent.
    ///
    /// \pre destination is aligned, disjoint, and large enough for owner.type(); owner
    /// is exclusively accessible and the destination retains complete destruction metadata.
    /// Leaves owner unit and does not destroy the transferred payload.
    /// Complexity: O(outer canonical bytes + released metadata D) time and O(1) auxiliary
    /// space; no allocation.
    static void release_to(value& owner, std::byte* destination) noexcept;
    /// Acquires outer storage and transfers a canonical owner without cloning its children.
    ///
    /// \pre data contains one exclusively accessible valid canonical owner of type.
    /// On success the caller retires source ownership without destroying it; failure
    /// preserves source ownership. The returned owner does not borrow source bytes.
    /// Complexity: O(outer canonical bytes) time and result storage.
    static value take(const value_type& type, const std::byte* data);
    /// Borrows initialized canonical bytes until the owner changes or dies.
    static const std::byte* data(const value& owner) noexcept;
    /// Copies initialized canonical bytes into a new independent public owner.
    ///
    /// \pre data is a readable valid owned representation of type whose descendants
    /// remain valid and unchanged during copying.
    /// Callable code retains explicit reference semantics; failure releases acquired owners.
    /// Complexity: O(B + N + L + C + D) time and new owned result storage, with costs
    /// as in value; O(1) runtime auxiliary space excluding opaque operations.
    static value copy(const value_type& type, const std::byte* data);
    /// Takes uniquely owned boundary elements into packed canonical storage.
    ///
    /// Sources become source unit as each element transfers ownership.
    /// Throws std::invalid_argument for shape/operation mismatch, destroying completed transfers.
    /// Remaining sources retain ownership.
    /// Throws std::overflow_error for canonical extent overflow before consuming sources.
    /// Complexity: O(B + F + M + D) expected time and packed result storage, with costs
    /// as in value and M the sum of compatibility comparison costs; comparisons use
    /// auxiliary space. Failure also retires consumed payloads.
    static value take_array(value_type element, std::span<value> children);
    /// Returns a packed array's logical length without acquiring ownership.
    ///
    /// \pre data contains a complete initialized array carrier with a live array owner.
    static std::size_t array_size(const std::byte* data) noexcept;
    /// Transfers the selected aggregate/array child and retires every sibling and outer owner.
    ///
    /// \pre type is an array, tuple, or record; source contains an exclusively accessible
    /// valid owner of type, index is in bounds, and destination is disjoint aligned
    /// uninitialized storage large enough for the selected child's canonical extent.
    /// The destination retains complete destruction metadata.
    /// Source ownership is retired on return without cloning the selected child.
    /// Source outer bytes remain caller-owned; separately owned array storage is released.
    /// Complexity: O(selected outer bytes + sibling F + N + L + C + D) time and
    /// O(1) runtime auxiliary space, with costs as in value.
    static void project(const value_type& type,
                        std::byte* source,
                        std::size_t index,
                        std::byte* destination) noexcept;
};

} // namespace cel::detail
#endif
