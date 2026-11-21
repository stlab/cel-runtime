// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_INSTRUCTION_HPP
#define CEL_RUNTIME_INSTRUCTION_HPP
#include <cel/runtime/detail/raw_segment.hpp>

namespace cel {
class compiled_segment;
} // namespace cel
namespace cel::detail {
class prepared_raw_segment;
/// Checked preparation and trusted canonical capture publication.
struct segment_access {
    /// Binds canonical execution for internal callers; follows the checked preparation contract.
    ///
    /// Complexity: compiled_segment's checked signature binding costs.
    static prepared_raw_segment prepare_raw(const compiled_segment& program,
                                            std::span<const value_type> arguments,
                                            const value_type& result);
    /// Releases a mutable sequence into the supplied retirement operation.
    static void retire(raw_segment& segment, retirement_queue& queue) noexcept;
    /// Releases frozen code into the supplied retirement operation without recursive destruction.
    static void retire(cel::compiled_segment& segment, retirement_queue& queue) noexcept;
    /// Releases a prepared binding's code, leaving its signature metadata owned by the binding.
    static void retire(cel::detail::prepared_raw_segment& segment,
                       retirement_queue& queue) noexcept;
    /// Stores a typed capture without requiring public value operands or fixed arity.
    ///
    /// F executes as const F&(frame&). Takes independent capture ownership; any referenced
    /// external objects retain their original ownership and lifetime obligations.
    ///
    /// \pre the callback enforces its complete ordered stack signature and cleans up partial
    /// result construction before propagating failure.
    ///
    /// Failure does not publish an operation or leave an unowned live capture.
    ///
    /// Complexity: callback construction and raw_segment append's amortized bookkeeping costs;
    /// owned capture storage.
    template <class F>
    static void append(raw_segment& segment, F callback) {
        static_assert(std::is_nothrow_destructible_v<F>);
        auto* location = static_cast<F*>(segment.allocate_capture(sizeof(F), alignof(F)));
        auto* capture = std::construct_at(location, std::move(callback));
        try {
            segment.append(
                capture,
                [](const void* object, frame& scope) {
                    std::invoke(*static_cast<const F*>(object), scope);
                },
                [](const void* object) noexcept { std::destroy_at(static_cast<const F*>(object)); },
                [](const void* object, retirement_queue& queue) noexcept {
                    if constexpr (requires(F & capture) { capture.retire(queue); }) {
                        static_assert(noexcept(std::declval<F&>().retire(queue)));
                        const_cast<F*>(static_cast<const F*>(object))->retire(queue);
                    }
                });
        } catch (...) {
            std::destroy_at(capture);
            throw;
        }
    }
};
} // namespace cel::detail
#endif
