// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_VALUE_OPS_HPP
#define CEL_RUNTIME_VALUE_OPS_HPP

#include <cel/runtime/detail/retirement.hpp>
#include <cel/runtime/value.hpp>

namespace cel::detail {
struct array_owner;
/// Guards a canonical packed array whose elements initialize in reverse source order.
/// Reuses the existing array owner and destruction operations; no erased move hook exists.
class array_builder {
    std::unique_ptr<array_owner> _owner;
    std::size_t _suffix = 0;

public:
    /// Allocates all packed element storage before ownership transfer.
    /// Throws overflow_error for count times canonical stride overflow.
    /// Complexity: O(element byte extent) time and packed storage.
    array_builder(const value_type& element, std::size_t count);
    /// Retires the initialized suffix with existing canonical ownership operations.
    /// Complexity: O(initialized elements + visited ownership paths); no allocation.
    ~array_builder();
    /// Disallows duplicated array ownership.
    array_builder(const array_builder&) = delete;
    /// Disallows duplicated assignment.
    array_builder& operator=(const array_builder&) = delete;
    /// Borrows the next uninitialized reverse-source element destination.
    /// Precondition: at least one uninitialized element remains.
    std::byte* destination() noexcept;
    /// Records complete initialization of the next reverse-source element.
    /// Precondition: destination() now contains exactly one complete owned element.
    void initialized() noexcept;
    /// Transfers a fully initialized canonical array carrier into external storage.
    /// Precondition: all elements are initialized; destination has array-carrier layout.
    /// Source ownership retires without cloning.
    void release_to(std::byte* destination) noexcept;
};

/// Clones one initialized raw value into uninitialized canonical storage.
/// Precondition: destination is an uninitialized, suitably aligned extent of type.
/// When opaque operations exist, source is a disjoint initialized canonical extent.
/// A deferred callable is rejected before source bytes are read.
/// Throws std::invalid_argument if callable ownership has not been installed.
/// Failure destroys every constructed child; destination remains uninitialized.
/// Complexity: O(canonical bytes + visited ownership paths) time and new owned space.
/// Traversal uses O(1) auxiliary space and worklist links in newly owned array nodes.
void clone_payload(const value_type& type, const std::byte* source, std::byte* destination);

/// Destroys an initialized raw value, retiring ownership without releasing its outer bytes.
/// Precondition: data contains exactly one valid owned representation of type.
/// Never allocates or throws. Bytewise transfer must retire the source without this call.
/// Complexity: O(visited ownership paths and array nodes) time and O(1) auxiliary space.
/// Scalar arrays need no per-element cleanup; nested arrays supply their own worklist links.
void destroy_payload(const value_type& type, std::byte* data) noexcept;
/// Retires a complete payload into an enclosing runtime executable retirement operation.
/// Precondition: data contains exactly one valid owned representation of type.
/// Complexity: O(visited ownership paths and array nodes); no allocation or throwing.
void destroy_payload(const value_type& type, std::byte* data, retirement_queue& queue) noexcept;

/// Bridges trusted owning wrappers to canonical raw representations.
/// Does not permit byte-relocation of the public wrapper itself.
struct value_access {
    /// Retires one complete public owner's managed children into the enclosing operation.
    /// Leaves owner as source unit; outer bytes and immutable descriptors retire normally.
    /// Complexity: O(visited ownership paths and array nodes); no allocation or throwing.
    static void retire(value& owner, retirement_queue& queue) noexcept;
    /// Transfers canonical ownership into a caller-provided uninitialized extent.
    /// Precondition: destination is aligned, disjoint, and large enough for owner.type().
    /// Leaves owner unit and does not destroy the transferred payload.
    /// Complexity: O(outer canonical bytes) time; no allocation.
    static void release_to(value& owner, std::byte* destination) noexcept;
    /// Acquires outer storage and transfers a canonical owner without cloning its children.
    /// Precondition: data contains one valid canonical owner of type.
    /// Success retires source ownership without destroying it; failure preserves source.
    /// Complexity: O(outer canonical bytes) time and space.
    static value take(const value_type& type, const std::byte* data);
    /// Borrows initialized canonical bytes until the owner changes or dies.
    static const std::byte* data(const value& owner) noexcept;
    /// Copies initialized canonical bytes into a new independent public owner.
    /// Precondition: data is a valid owned representation of type.
    /// Complexity: O(payload size) time and new owned space.
    static value copy(const value_type& type, const std::byte* data);
    /// Takes uniquely owned boundary elements into packed canonical storage.
    /// Sources become source unit as each element transfers ownership.
    /// Throws std::invalid_argument for shape/operation mismatch, destroying completed transfers.
    /// Remaining sources retain ownership.
    /// Throws std::overflow_error for canonical extent overflow before consuming sources.
    /// Complexity: O(canonical element bytes + compared shape size) time and packed outer storage.
    static value take_array(value_type element, std::span<value> children);
    /// Returns a packed array's logical length without acquiring ownership.
    /// Precondition: data contains a complete initialized array carrier.
    static std::size_t array_size(const std::byte* data) noexcept;
    /// Transfers the selected aggregate/array child and retires every sibling and outer owner.
    /// Precondition: index is in bounds; destination is disjoint aligned uninitialized storage.
    /// Source ownership is retired on return.
    /// Complexity: O(sibling count + visited ownership paths + selected outer bytes).
    static void project(const value_type& type,
                        std::byte* source,
                        std::size_t index,
                        std::byte* destination) noexcept;
};

} // namespace cel::detail
#endif
