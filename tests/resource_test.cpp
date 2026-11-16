// SPDX-License-Identifier: BSL-1.0
#include "type_ops.hpp"
#include "value_ops.hpp"
#include <cel/runtime/value.hpp>

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <latch>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

/// Runs two nonthrowing callbacks on separate threads behind one start gate and joins them.
///
/// Thread-start failures propagate after releasing any already started worker.
/// Complexity: callback work plus thread startup and joining; O(1) auxiliary space.
template <class First, class Second>
void run_concurrently(First first, Second second) {
    std::latch start{1};
    std::array<std::jthread, 2> threads;
    const auto release = [](std::latch* gate) noexcept { gate->count_down(); };
    const std::unique_ptr<std::latch, decltype(release)> release_on_exit(&start, release);
    threads[0] = std::jthread([&] {
        start.wait();
        first();
    });
    threads[1] = std::jthread([&] {
        start.wait();
        second();
    });
}

/// Records actual node disposal and reentrant callbacks across independent retirement queues.
struct retirement_counts {
    std::atomic<std::size_t> disposed{0}; ///< Counts destroyed nodes.
    std::atomic<bool> nested{false};      ///< Records nesting within one thread.
};

/// An owned chain node for checking the retirement primitive used by type metadata.
struct retirement_probe : cel::detail::retirement_node {
    retirement_counts& counts; ///< Borrows counters until every worker has joined.
    cel::detail::retirement_ptr<retirement_probe> child; ///< Retains the next chain node.
    static inline thread_local std::size_t depth = 0; ///< Counts active callbacks on this thread.

    /// Adopts one child edge and retains the disposal counters.
    retirement_probe(retirement_counts& state, cel::detail::retirement_ptr<retirement_probe> next) :
        retirement_node(dispose_node), counts(state), child(std::move(next)) {}

    /// Records disposal, detaches the child into queue, and deletes this node.
    ///
    /// \pre base is an exclusively retiring retirement_probe.
    static void dispose_node(cel::detail::retirement_node& base,
                             cel::detail::retirement_queue& queue) noexcept {
        auto& node = static_cast<retirement_probe&>(base);
        ++depth;
        if (depth != 1) node.counts.nested.store(true, std::memory_order_relaxed);
        node.child.retire(queue);
        node.counts.disposed.fetch_add(1, std::memory_order_relaxed);
        delete &node;
        --depth;
    }
};

/// Catches premature disposal or wrong last-owner selection during concurrent edge releases.
TEST_CASE("concurrent last owners retire each node once without nested callbacks") {
    constexpr std::size_t count = 12000;
    retirement_counts counts;
    cel::detail::retirement_ptr<retirement_probe> root;
    CHECK(root.get() == nullptr);
    std::vector<cel::detail::retirement_ptr<retirement_probe>> aliases;
    // Retain interior aliases so independent queues also compete below the root.
    for (std::size_t i = 0; i != count; ++i) {
        auto next = std::make_unique<retirement_probe>(counts, std::move(root));
        root = cel::detail::retirement_ptr<retirement_probe>(next.release());
        if (i % 2 == 0) aliases.push_back(root);
    }
    {
        auto copy = root;
        CHECK(copy == root);
        CHECK(copy.get() == root.get());
        auto moved = std::move(copy);
        // NOLINTNEXTLINE(bugprone-use-after-move): moves promise an empty source handle.
        CHECK_FALSE(copy);
        CHECK(moved == root);
    }
    CHECK(counts.disposed.load() == 0);
    run_concurrently([owner = std::move(root)]() mutable { owner = {}; },
                     [owners = std::move(aliases)]() mutable {
                         while (!owners.empty())
                             owners.pop_back();
                     });
    CHECK(counts.disposed.load() == count);
    CHECK_FALSE(counts.nested.load());
}

/// Catches disposing metadata before its last-owner queue drains.
TEST_CASE("queued metadata retains borrowed views until disposal") {
    const auto boolean = cel::value_type::leaf<bool>();
    auto type = cel::value_type::record(
        "queued", std::array{cel::record_field{"items", cel::value_type::array(boolean)},
                             cel::record_field{"code", cel::value_type::callable(
                                                           std::array{boolean},
                                                           cel::value_type::leaf<double>())}});
    const auto fields = type.fields();
    const auto names = type.field_names();
    const auto identity = type.record_identity();
    const auto& element = fields[0].element();
    const auto parameters = fields[1].parameters();
    const auto& result = fields[1].result();
    cel::detail::retirement_queue queue;
    cel::detail::type_access::retire(type, queue);
    CHECK(identity == "queued");
    CHECK(names[0] == "items");
    CHECK(names[1] == "code");
    CHECK(element.same_shape(boolean));
    CHECK(parameters[0].same_shape(boolean));
    CHECK(result.same_shape(cel::value_type::leaf<double>()));
}

/// Adds an array, shared aggregate, or callable-result edge.
///
/// Complexity: O(1) time and new metadata space.
cel::value_type extend_metadata(const cel::value_type& child, std::size_t level) {
    switch (level % 4) {
        case 0:
            return cel::value_type::array(child);
        case 1:
            return cel::value_type::tuple(std::array{child, child});
        case 2:
            return cel::value_type::record("shared", std::array{cel::record_field{"left", child},
                                                                cel::record_field{"right", child}});
        default:
            return cel::value_type::callable({}, child);
    }
}

/// Catches invalidating retained metadata during concurrent root and interior-alias release.
TEST_CASE("concurrent metadata release preserves retained shared descendants") {
    constexpr std::size_t depth = 12000;
    auto type = cel::value_type::leaf<bool>();
    auto expected = cel::value_type::leaf<bool>();
    std::vector<cel::value_type> aliases;
    // Construct a second, independent shape for the descendant that survives retirement.
    for (std::size_t i = 0; i != depth / 2; ++i)
        expected = extend_metadata(expected, i);
    for (std::size_t i = 0; i != depth; ++i) {
        type = extend_metadata(type, i);
        aliases.push_back(type);
    }
    const auto survivor = aliases[depth / 2 - 1];
    std::vector<cel::value_type> roots{type};
    type = cel::value_type{};
    run_concurrently([owners = std::move(roots)]() mutable { owners.clear(); },
                     [owners = std::move(aliases)]() mutable {
                         while (!owners.empty())
                             owners.pop_back();
                     });
    CHECK(survivor.same_shape(expected));
    CHECK(survivor.result().kind() == cel::type_kind::record);
}

/// Checks every carrier offset and a linear bound on constant-time traversal steps.
/// Complexity: O(count log count) time and O(count) temporary space.
void check_ownership_traversal(const cel::value_type& type, std::size_t count) {
    cel::detail::ownership_cursor cursor(type);
    std::vector<std::size_t> offsets;
    std::size_t steps = 0;
    while (!cursor.empty()) {
        if (const auto slot = cursor.next()) {
            CHECK((slot->type->kind() == cel::type_kind::array));
            offsets.push_back(slot->offset);
        }
        ++steps;
    }
    REQUIRE((offsets.size() == count));
    CHECK((steps <= 3 * count));
    std::sort(offsets.begin(), offsets.end());
    for (std::size_t i = 0; i != count; ++i)
        CHECK((offsets[i] == i * sizeof(void*)));
}

/// Catches repeated root searches and expanded metadata at every skewed descriptor.
TEST_CASE("ownership ranges traverse wide and skewed aggregates in linear work") {
    constexpr std::size_t count = 2048;
    const auto element = cel::value::from(std::vector<double>{3.0});
    const cel::value_type unit;
    const std::vector<cel::value> fields(count, element);
    const auto flat = cel::value::tuple(fields);
    check_ownership_traversal(flat.type(), count);

    for (const bool heavy_first : {false, true}) {
        auto type = element.type();
        std::size_t ranges = 0;
        std::size_t range_bytes = 0;
        for (std::size_t i = 1; i != count; ++i) {
            const auto children =
                heavy_first ? std::array{type, element.type()} : std::array{element.type(), type};
            type = cel::value_type::tuple(children);
            const auto compiled = cel::detail::type_access::ownership_ranges(type);
            ranges += compiled.size();
            range_bytes += compiled.size_bytes();
        }
        CHECK(ranges <= 2 * count);
        CHECK(range_bytes <= 2 * count * sizeof(cel::detail::ownership_slot));
        check_ownership_traversal(type, count);
        auto original =
            cel::detail::value_access::copy(type, cel::detail::value_access::data(flat));
        auto copy = original;
        original = cel::value{};
        auto restored =
            cel::detail::value_access::copy(flat.type(), cel::detail::value_access::data(copy));
        CHECK(restored.at(0).at(0).as<double>() == 3.0);
        CHECK(restored.at(count - 1).at(0).as<double>() == 3.0);
        copy = cel::value{};
        restored = cel::value{};
        type = unit;
    }
}

/// Supplies a nonowning client field with extended alignment.
struct alignas(256) ownership_aligned_leaf {
    std::array<std::byte, 256> bytes; ///< Stores a trivially copyable opaque payload.
};

/// Catches incorrect range offsets when zero-byte and over-aligned fields precede carriers.
TEST_CASE("record ownership ranges preserve canonical offsets around nonowning fields") {
    const auto array = cel::value::from(std::vector<double>{5.0});
    const auto wide = cel::value::from(ownership_aligned_leaf{});
    const auto type =
        cel::value_type::record("mixed", std::array{cel::record_field{"unit", cel::value_type{}},
                                                    cel::record_field{"bytes", wide.type()},
                                                    cel::record_field{"items", array.type()}});
    auto original = cel::value::record(type, std::array{cel::value{}, wide, array});
    auto copy = original;
    copy.set_field("items", cel::value::from(std::vector<double>{9.0}));
    CHECK(original.field("items").at(0).as<double>() == 5.0);
    CHECK(copy.field("items").at(0).as<double>() == 9.0);
    original = cel::value{};
    copy = cel::value{};
}

/// Catches expansion of shared ranges and traversal proportional to unary nesting.
TEST_CASE("ownership ranges reuse shared descendants and collapse unary paths") {
    auto type = cel::value_type::array(cel::value_type::leaf<double>());
    std::size_t ranges = 0;
    for (std::size_t i = 0; i != 12; ++i) {
        type = cel::value_type::tuple(std::array{type, type});
        ranges += cel::detail::type_access::ownership_ranges(type).size();
    }
    CHECK(ranges <= 24);
    check_ownership_traversal(type, 4096);
    for (std::size_t i = 0; i != 50000; ++i)
        type = cel::value_type::tuple(std::array{type});
    check_ownership_traversal(type, 4096);
}
/// Catches recursive last-reference release, including during exception cleanup.
TEST_CASE("deep descriptor release and exception unwinding are stack independent") {
    const cel::value_type unit;
    {
        auto type = cel::value_type::leaf<bool>();
        for (std::size_t i = 0; i != 50000; ++i)
            type = cel::value_type::array(type);
        type = unit;
        CHECK(type.same_shape(unit));
    }
    CHECK_THROWS_AS(
        [&] {
            auto type = cel::value_type::leaf<bool>();
            for (std::size_t i = 0; i != 50000; ++i)
                type = cel::value_type::array(type);
            throw std::runtime_error("unwind deep metadata");
        }(),
        std::runtime_error);
}

/// Catches exponential admission of compact shared zero-byte tuple metadata.
TEST_CASE("compact tuple DAG parameters do not expand their shared subgraphs") {
    cel::value_type type;
    cel::value_type equal;
    for (std::size_t i = 0; i != 27; ++i) {
        type = cel::value_type::tuple(std::array{type, type});
        equal = cel::value_type::tuple(std::array{equal, equal});
    }
    const auto callable =
        cel::value_type::callable(std::array{type}, cel::value_type::leaf<bool>());
    CHECK(callable.parameters()[0].same_shape(equal));
    CHECK(type.byte_layout().size == 0);
}

/// Catches recursive comparison or callable validation of deeply admitted metadata.
TEST_CASE("deep metadata comparison and parameter admission are stack independent") {
    auto a = cel::value_type::leaf<bool>();
    auto b = cel::value_type::leaf<bool>();
    for (std::size_t i = 0; i != 50000; ++i) {
        a = cel::value_type::array(a);
        b = cel::value_type::array(b);
    }
    CHECK(a.same_shape(b));
    CHECK(
        cel::value_type::callable(std::array{a}, cel::value_type{}).parameters()[0].same_shape(b));
}

/// Verifies scalar array conversion and independent copies at different sizes.
TEST_CASE("scalar canonical arrays preserve values and independent copies") {
    const std::vector<double> small_input(16, 2.0);
    const std::vector<double> large_input(4096, 3.0);
    auto small = cel::value::from(small_input);
    auto large = cel::value::from(large_input);
    CHECK(small.as<std::vector<double>>() == small_input);
    auto clone = large;
    clone.set(0, cel::value::from(7.0));
    CHECK(large.at(0).as<double>() == 3.0);
    CHECK(clone.at(4095).as<double>() == 3.0);
    const auto scalar = cel::value::from(2.0);
    const std::vector<cel::value> small_children(16, scalar);
    const std::vector<cel::value> large_children(4096, scalar);
    small = cel::value::array(scalar.type(), small_children);
    large = cel::value::array(scalar.type(), large_children);
    CHECK(small.size() == small_children.size());
    CHECK(large.size() == large_children.size());
    CHECK(large.at(4095).as<double>() == 2.0);
}

/// Verifies canonical tuple array conversion at different sizes.
TEST_CASE("nonowning tuple array elements preserve their values") {
    const std::vector<std::tuple<bool, double>> small_input(16, {true, 2.0});
    const std::vector<std::tuple<bool, double>> large_input(4096, {false, 3.0});
    const auto small = cel::value::from(small_input);
    const auto large = cel::value::from(large_input);
    CHECK(small.as<std::vector<std::tuple<bool, double>>>() == small_input);
    CHECK(large.at(4095).as<std::tuple<bool, double>>() == std::tuple{false, 3.0});
}

/// Catches recursive cloning and destruction of owned array trees.
TEST_CASE("deep owned array trees clone release and unwind without recursive cleanup") {
    auto source = cel::value::from(true);
    for (std::size_t i = 0; i != 50000; ++i)
        source = cel::detail::value_access::take_array(source.type(), std::span(&source, 1));
    auto copy = source;
    CHECK(copy.type().same_shape(source.type()));
    CHECK(copy.size() == 1);
    copy = cel::value{};
    source = cel::value{};
    CHECK_THROWS_AS(
        [] {
            auto tree = cel::value::from(false);
            for (std::size_t i = 0; i != 50000; ++i)
                tree = cel::detail::value_access::take_array(tree.type(), std::span(&tree, 1));
            throw std::runtime_error("unwind packed ownership");
        }(),
        std::runtime_error);
}

/// Catches recursion through deeply nested inline fields even when only one carrier exists.
TEST_CASE("deep inline aggregate ownership paths are stack independent") {
    const auto leaf_array = cel::value::from(std::vector<double>{2.0, 3.0});
    auto type = leaf_array.type();
    for (std::size_t i = 0; i != 50000; ++i)
        type = cel::value_type::tuple(std::array{type});
    auto owner = cel::detail::value_access::copy(type, cel::detail::value_access::data(leaf_array));
    auto copy = owner;
    CHECK(copy.type().same_shape(type));
    owner = cel::value{};
    copy = cel::value{};
}
