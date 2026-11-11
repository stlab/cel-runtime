// SPDX-License-Identifier: BSL-1.0
#include <cel/runtime.hpp>
#include <exception>
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

/// Identifies the original nonallocating exception used by retirement unwinding tests.
struct unwind_error : std::exception {
    int tag = 17; ///< Preserves the caller's exception witness.
    /// Returns the original fixed diagnostic.
    const char* what() const noexcept override { return "retirement unwind witness"; }
};

/// Creates a public higher-order literal ownership chain.
/// Complexity: O(depth + descriptor binding work) time and owned space.
cel::value literal_chain(int depth) {
    cel::dyn_segment base;
    base.just(cel::value::from(true));
    auto owner = cel::callable(std::move(base).finish(), {}).to_value();
    for (int i = 0; i != depth; ++i) {
        cel::dyn_segment next;
        next.just(std::move(owner));
        owner = cel::callable(std::move(next).finish(), {}).to_value();
    }
    return owner;
}

/// Creates lazy executable ownership without evaluating nested branches.
/// Complexity: O(depth) time and owned space.
cel::compiled_segment lazy_chain(int depth) {
    cel::dyn_segment base;
    base.just(cel::value::from(true));
    for (int i = 0; i != depth; ++i) {
        cel::dyn_segment next;
        next.just(cel::value::from(true));
        next.logical_and(std::move(base));
        base = std::move(next);
    }
    return std::move(base).finish();
}

TEST_CASE("deep callable literals retire") {
    auto owner = literal_chain(12000);
    CHECK_NOTHROW(owner = cel::value{});
}

TEST_CASE("deep callable literals retire while preserving an unwinding exception") {
    bool caught = false;
    try {
        auto owner = literal_chain(12000);
        auto retired = std::move(owner);
        throw unwind_error{};
    } catch (const unwind_error& error) {
        caught = error.tag == 17;
    }
    CHECK(caught);
}

TEST_CASE("deep lazy captures retire") {
    auto program = lazy_chain(12000);
    cel::dyn_segment empty;
    auto replacement = std::move(empty).finish();
    CHECK_NOTHROW(program = std::move(replacement));
}

TEST_CASE("shared executable children survive another parent's retirement") {
    auto owner = literal_chain(12000);
    auto survivor = owner;
    owner = cel::value{};
    {
        auto fn = cel::callable::from_value(survivor);
        cel::raw_stack stack;
        CHECK(fn.call(stack, {}).type().same_shape(fn.type().result()));
    }
    CHECK_NOTHROW(survivor = cel::value{});
}

TEST_CASE("array tuple and executable ownership edges share one retirement operation") {
    const auto create = [] {
        auto child = literal_chain(12000);
        const std::array elements{child};
        const std::array fields{cel::value::array(child.type(), elements)};
        cel::dyn_segment body;
        body.just(cel::value::tuple(fields));
        return cel::callable(std::move(body).finish(), {}).to_value();
    };
    auto root = create();
    CHECK_NOTHROW(root = cel::value{});
}

TEST_CASE("deep lazy code retained by a prepared binding retires on its last release") {
    auto program = lazy_chain(12000);
    auto prepared = program.prepare_raw({}, cel::value_type::leaf<bool>());
    cel::dyn_segment empty;
    auto replacement = std::move(empty).finish();
    auto replacement_binding = replacement.prepare_raw({}, cel::value_type{});
    program = std::move(replacement);
    CHECK_NOTHROW(prepared = std::move(replacement_binding));
}
