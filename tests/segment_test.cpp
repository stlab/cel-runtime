// SPDX-License-Identifier: BSL-1.0
#include <cel/runtime/dyn_segment.hpp>
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <array>
#include <doctest/doctest.h>
#include <limits>
#include <memory>
#include <tuple>
#include <vector>

/// Exercises root admission independently of callable-parameter restrictions.
struct alignas(256) client_leaf {
    int number; ///< Retains a client-defined scalar.
};

/// Holds one nested nominal record for typed-operation adapter tests.
struct segment_bounds {
    /// Stores the horizontal extent.
    double width;
    /// Stores the vertical extent.
    double height;
};

/// Holds one nominal record consumed by typed callbacks.
struct segment_widget {
    /// Records whether the widget is active.
    bool enabled;
    /// Records nested bounds.
    segment_bounds bounds;
};

/// Holds coordinates in an order different from its canonical record schema.
struct segment_reordered_point {
    /// Stores the vertical coordinate.
    double y;
    /// Stores the horizontal coordinate.
    double x;
};

namespace cel {
template <>
struct record_adapter<segment_bounds> {
    /// Returns the complete nested descriptor.
    static value_type type() {
        static const auto descriptor = value_type::record(
            "tests.segment_bounds", std::array{record_field{"width", value_type::leaf<double>()},
                                               record_field{"height", value_type::leaf<double>()}});
        return descriptor;
    }
    /// Encodes one nested record value.
    static value to_value(const segment_bounds& source) {
        return value::record(type(),
                             std::array{value::from(source.width), value::from(source.height)});
    }
    /// Decodes one nested record value.
    static segment_bounds from_value(const value& source) {
        if (!source.type().same_shape(type()))
            throw std::invalid_argument("wrong segment bounds type");
        return {.width = source.field("width").as<double>(),
                .height = source.field("height").as<double>()};
    }
};

template <>
struct record_adapter<segment_widget> {
    /// Returns the complete nominal descriptor.
    static value_type type() {
        static const auto descriptor = value_type::record(
            "tests.segment_widget",
            std::array{record_field{"enabled", value_type::leaf<bool>()},
                       record_field{"bounds", record_adapter<segment_bounds>::type()}});
        return descriptor;
    }
    /// Encodes one nominal record value.
    static value to_value(const segment_widget& source) {
        return value::record(type(),
                             std::array{value::from(source.enabled), value::from(source.bounds)});
    }
    /// Decodes one nominal record value.
    static segment_widget from_value(const value& source) {
        if (!source.type().same_shape(type()))
            throw std::invalid_argument("wrong segment widget type");
        return {.enabled = source.field("enabled").as<bool>(),
                .bounds = source.field("bounds").as<segment_bounds>()};
    }
};

template <>
struct record_adapter<::segment_reordered_point> {
    /// Returns the canonical coordinate schema.
    static value_type type() {
        static const auto descriptor =
            value_type::record("tests.segment_reordered_point",
                               std::array{record_field{"x", value_type::leaf<double>()},
                                          record_field{"y", value_type::leaf<double>()}});
        return descriptor;
    }
    /// Encodes coordinates by their names rather than their native order.
    static value to_value(const ::segment_reordered_point& source) {
        return value::record(type(), std::array{value::from(source.x), value::from(source.y)});
    }
    /// Decodes coordinates by their canonical names.
    static ::segment_reordered_point from_value(const value& source) {
        if (!source.type().same_shape(type()))
            throw std::invalid_argument("wrong reordered point type");
        return {.y = source.field("y").as<double>(), .x = source.field("x").as<double>()};
    }
};

} // namespace cel

/// Catches byte-based counting, reversed suffixes, and inclusion of ambient operands.
TEST_CASE("operand types borrow the older-first local logical suffix") {
    cel::dyn_segment empty;
    CHECK(empty.operand_types(0).empty());
    CHECK(empty.operand_types(1).empty());
    CHECK(empty.operand_types(std::numeric_limits<std::size_t>::max()).empty());
    cel::dyn_segment builder(cel::stack_origin{128, 7});
    CHECK(builder.operand_types(1).empty());
    builder.just(cel::value::from(3.0));
    builder.just(cel::value{});
    builder.just(cel::value::from(true));
    const auto operands = builder.operand_types(3);
    REQUIRE(operands.size() == 3);
    CHECK(operands[0].same_shape(cel::value_type::leaf<double>()));
    CHECK(operands[1].same_shape(cel::value_type{}));
    CHECK(operands[2].same_shape(cel::value_type::leaf<bool>()));
    const auto suffix = builder.operand_types(2);
    REQUIRE(suffix.size() == 2);
    CHECK(suffix.data() == operands.data() + 1);
    CHECK(suffix[0].byte_layout().size == 0);
    CHECK(builder.operand_types(1).data() == operands.data() + 2);
    CHECK(builder.operand_types(0).empty());
    CHECK(builder.operand_types(4).empty());
    CHECK(builder.origin() == cel::stack_origin{128, 7});
}

/// Catches opcode publication, callback execution, or consumed operands during inspection.
TEST_CASE("operand inspection leaves code types and callback effects unchanged") {
    static_assert(noexcept(std::declval<const cel::dyn_segment&>().operand_types(1)));
    static_assert(std::same_as<decltype(std::declval<const cel::dyn_segment&>().operand_types(1)),
                               std::span<const cel::value_type>>);
    int effects = 0;
    cel::dyn_segment builder;
    builder.op0<double>([&effects] {
        ++effects;
        return 10.0;
    });
    builder.just(cel::value::from(3.0));
    const auto before = builder.snapshot();
    const auto original = builder.operand_types(2);
    const auto observed = builder.operand_types(2);
    (void)builder.operand_types(3);
    (void)builder.operand_types(0);
    CHECK(effects == 0);
    CHECK(builder.snapshot() == before);
    REQUIRE(observed.size() == 2);
    CHECK(observed.data() == original.data());
    CHECK(observed[0].compatible_with(cel::value_type::leaf<double>()));
    CHECK(observed[1].compatible_with(cel::value_type::leaf<double>()));
    builder.op2<double, double, double>([](double older, double newer) { return older - newer; });
    auto program = std::move(builder).finish();
    cel::raw_stack stack;
    CHECK(program.execute(stack, {}).as<double>() == 7.0);
    CHECK(effects == 1);
    CHECK(program.execute(stack, {}).as<double>() == 7.0);
    CHECK(effects == 2);
    CHECK(stack.depth() == 0);
}

/// Catches descriptors that borrow builder-owned plans instead of retaining immutable metadata.
TEST_CASE("copied operand descriptors survive mutation move and builder destruction") {
    const auto retained = [] {
        cel::dyn_segment builder;
        builder.just(cel::value::from(std::tuple{std::vector<double>{2.0}, true}));
        auto copy = builder.operand_types(1).front();
        builder.project_tuple(0);
        auto moved = std::move(builder);
        moved.just(cel::value::from(false));
        return copy;
    }();
    const auto expected = cel::value_type::tuple(std::array{
        cel::value_type::array(cel::value_type::leaf<double>()), cel::value_type::leaf<bool>()});
    CHECK(retained.compatible_with(expected));
    CHECK(retained.owns_values());
}

TEST_CASE("dynamic ordered mixed operations freeze reads and literals") {
    cel::dyn_segment builder;
    builder.push_argument(1, cel::value_type::leaf<double>());
    builder.just(cel::value::from(true));
    builder.push_argument(0, cel::value_type::leaf<double>());
    builder.op3<double, double, bool, double>(
        [](double older, bool flag, double newer) { return flag ? older - newer : 0.0; });
    auto program = std::move(builder).finish();
    const std::array inputs{cel::value::from(2.0), cel::value::from(10.0)};
    cel::raw_stack stack;
    CHECK(program.execute(stack, inputs).as<double>() == 8.0);
    auto typed = program.prepare<double, double, double>();
    CHECK(typed.call(stack, 3.0, 12.0) == 9.0);
    CHECK(stack.depth() == 0);
}

TEST_CASE("checked signatures reject errors before callback effects") {
    int effects = 0;
    cel::dyn_segment builder;
    builder.op0<double>([&effects] {
        ++effects;
        return 2.0;
    });
    builder.push_argument(1, cel::value_type::leaf<double>());
    builder.op2<double, double, double>([](double a, double b) { return a + b; });
    auto program = std::move(builder).finish();
    cel::raw_stack stack;
    const std::array wrong{cel::value::from(2.0), cel::value::from(true)};
    CHECK_THROWS_AS(program.execute(stack, wrong), std::invalid_argument);
    CHECK_THROWS_AS(program.execute(stack, {}), std::invalid_argument);
    CHECK_THROWS_AS((program.prepare<double, double>()), std::invalid_argument);
    CHECK_THROWS_AS((program.prepare<bool, double, double>()), std::invalid_argument);
    CHECK(effects == 0);
    CHECK(stack.depth() == 0);
}

TEST_CASE("construction rejects underflow mismatches and conflicting reads") {
    cel::dyn_segment builder;
    CHECK_THROWS_AS((builder.op1<double, double>([](double n) { return n; })),
                    std::invalid_argument);
    builder.push_argument(0, cel::value_type::leaf<bool>());
    CHECK_THROWS_AS((builder.op1<double, double>([](double n) { return n; })),
                    std::invalid_argument);
    CHECK_THROWS_AS(builder.push_argument(0, cel::value_type::leaf<double>()),
                    std::invalid_argument);
    auto program = std::move(builder).finish();
    cel::raw_stack stack;
    CHECK(program.prepare<bool, bool>().call(stack, true));
}

TEST_CASE("frozen captures and literals outlive their builders and sources") {
    auto prepared = [] {
        cel::dyn_segment builder;
        auto literal = cel::value::from(std::vector{2.0, 3.0});
        builder.just(std::move(literal));
        builder.op1<std::vector<double>, std::vector<double>>(
            [factor = std::make_unique<double>(4.0)](std::vector<double> numbers) {
                numbers[0] *= *factor;
                return numbers;
            });
        return std::move(builder).finish().prepare<std::vector<double>>();
    }();
    cel::raw_stack stack;
    auto first = prepared.call(stack);
    first[0] = 99.0;
    CHECK(prepared.call(stack) == std::vector<double>{8.0, 3.0});
    CHECK(first[0] == 99.0);
}
/// Owns an over-aligned factor that can be copied but not moved.
struct alignas(256) copy_only_factor {
    double factor; ///< Owns the multiplication factor.
    /// Initializes the factor.
    explicit copy_only_factor(double input) : factor(input) {}
    /// Copies the factor independently.
    copy_only_factor(const copy_only_factor&) = default;
    /// Disallows move construction.
    copy_only_factor(copy_only_factor&&) = delete;
    /// Multiplies the operand by the owned factor.
    double operator()(double input) const { return factor * input; }
};

/// Owns a factor with converting construction distinct from ordinary copying.
struct adaptable_factor {
    double factor; ///< Owns the multiplication factor.
    /// Initializes the factor.
    explicit adaptable_factor(double input) : factor(input) {}
    /// Initializes zero from a source of another type.
    template <class T>
    explicit adaptable_factor(const T&) : factor(0.0) {}
    /// Multiplies the operand by the owned factor.
    double operator()(double input) const { return factor * input; }
};

TEST_CASE("dynamic and selected callbacks copy nonmovable lvalues independently") {
    copy_only_factor callback(3.0);
    cel::dyn_segment dynamic;
    dynamic.just(cel::value::from(2.0));
    dynamic.op1<double, double>(callback);
    auto selected = cel::with_signature<double(double)>(callback);
    cel::dyn_segment explicit_signature;
    explicit_signature.just(cel::value::from(4.0));
    explicit_signature.op1<double, double>(selected);
    callback.factor = 99.0;
    cel::raw_stack stack;
    CHECK(std::move(dynamic).finish().prepare<double>().call(stack) == 6.0);
    CHECK(std::move(explicit_signature).finish().prepare<double>().call(stack) == 12.0);
}

TEST_CASE("selected callable wrappers preserve independent lvalue copies") {
    auto selected = cel::with_signature<double(double)>(adaptable_factor(2.0));
    auto copied = selected;
    decltype(selected) direct(selected);
    selected = cel::with_signature<double(double)>(adaptable_factor(3.0));
    cel::dyn_segment builder;
    builder.just(cel::value::from(4.0));
    builder.op1<double, double>(copied);
    cel::raw_stack stack;
    CHECK(std::move(builder).finish().prepare<double>().call(stack) == 8.0);
    CHECK(direct(4.0) == 8.0);
    CHECK(selected(4.0) == 12.0);
}

TEST_CASE("valid over-aligned client root arguments and results are admitted") {
    cel::dyn_segment builder;
    builder.push_argument(0, cel::value_type::leaf<client_leaf>());
    builder.op1<client_leaf, client_leaf>(
        [](const client_leaf& source) { return client_leaf{source.number + 1}; });
    auto prepared = std::move(builder).finish().prepare<client_leaf, client_leaf>();
    cel::raw_stack stack;
    stack.reserve(2048, 256);
    CHECK(prepared.call(stack, client_leaf{41}).number == 42);
}

TEST_CASE("finalization rejects silent leftover operand discard") {
    cel::dyn_segment builder;
    builder.just(cel::value::from(true));
    builder.just(cel::value::from(2.0));
    CHECK_THROWS_AS(std::move(builder).finish(), std::invalid_argument);
}
TEST_CASE("typed operations accept nominal record adapters and propagate callback failures") {
    CHECK(cel::native_type<segment_widget>().record_identity() == "tests.segment_widget");
    bool fail = true;
    cel::dyn_segment builder;
    builder.push_argument(0, cel::native_type<segment_widget>());
    builder.op1<segment_widget, segment_widget>([&fail](segment_widget value) {
        if (fail) throw std::runtime_error("callback");
        value.enabled = !value.enabled;
        value.bounds.width += 1.0;
        value.bounds.height += 2.0;
        return value;
    });
    auto prepared = std::move(builder).finish().prepare<segment_widget, segment_widget>();
    const segment_widget input{.enabled = true, .bounds = {.width = 3.0, .height = 5.0}};
    cel::raw_stack stack;
    stack.push(std::vector{7.0});
    const auto cursor = stack.cursor();
    CHECK_THROWS_AS(prepared.call(stack, input), std::runtime_error);
    CHECK(stack.depth() == 1);
    CHECK(stack.cursor() == cursor);
    fail = false;
    const auto result = prepared.call(stack, input);
    CHECK_FALSE(result.enabled);
    CHECK(result.bounds.width == doctest::Approx(4.0));
    CHECK(result.bounds.height == doctest::Approx(7.0));
    CHECK(stack.pop<std::vector<double>>() == std::vector<double>{7.0});
}

TEST_CASE("typed record operands preserve canonical field order") {
    cel::dyn_segment builder;
    builder.push_argument(0, cel::native_type<segment_reordered_point>());
    builder.op1<segment_reordered_point, segment_reordered_point>(
        [](segment_reordered_point point) { return point; });
    auto prepared =
        std::move(builder).finish().prepare<segment_reordered_point, segment_reordered_point>();
    cel::raw_stack stack;
    const segment_reordered_point input{.y = 2.0, .x = 1.0};
    const auto result = prepared.call(stack, input);
    CHECK(result.x == doctest::Approx(1.0));
    CHECK(result.y == doctest::Approx(2.0));
}

TEST_CASE("typed calls accept unused composite arguments") {
    cel::dyn_segment builder;
    builder.op0<double>([] { return 5.0; });
    auto bound = std::move(builder).finish().prepare<double, std::vector<double>>();
    cel::raw_stack stack;
    stack.reserve(256, 256);
    const std::vector<double> unused(100, 7.0);
    CHECK(bound.call(stack, unused) == 5.0);
}
TEST_CASE("arbitrary arity repeated reads and reference results remain exact") {
    cel::dyn_segment builder;
    builder.push_argument(0, cel::value_type::leaf<double>());
    builder.push_argument(0, cel::value_type::leaf<double>());
    builder.op0<bool>([] { return true; });
    builder.op0<double>([] { return 2.0; });
    builder.op<double, double, double, bool, double>(
        [](double a, double b, bool flag, double c) { return flag ? a + b + c : 0.0; });
    builder.op1<double, double>([](double n) { return n + 1.0; });
    auto bound = std::move(builder).finish().prepare<double, double>();
    cel::raw_stack stack;
    CHECK(bound.call(stack, 3.0) == 9.0);
    cel::dyn_segment reference;
    const std::vector<double> source{3.0};
    reference.op0<std::vector<double>>(
        [&source]() -> const std::vector<double>& { return source; });
    auto copied = std::move(reference).finish().prepare<std::vector<double>>().call(stack);
    copied[0] = 7.0;
    CHECK(source[0] == 3.0);
}
TEST_CASE("ordinary callbacks execute outside acquisition and retirement gates") {
    cel::dyn_segment nested_builder;
    nested_builder.op0<double>([] { return 4.0; });
    auto nested = std::move(nested_builder).finish().prepare<double>();
    cel::raw_stack stack;
    stack.push(std::vector{9.0});
    cel::dyn_segment builder;
    builder.push_argument(0, cel::value_type::leaf<double>());
    builder.op1<double, double>([&nested, &stack](double n) { return nested.call(stack) + n; });
    auto bound = std::move(builder).finish().prepare<double, double>();
    CHECK(bound.call(stack, 3.0) == 7.0);
    CHECK(stack.pop<std::vector<double>>() == std::vector<double>{9.0});
}

TEST_CASE("typed composite results retain caller prefix") {
    cel::dyn_segment builder;
    builder.op0<std::vector<double>>([] { return std::vector<double>{3.0}; });
    auto bound = std::move(builder).finish().prepare<std::vector<double>>();
    cel::raw_stack stack;
    stack.reserve(4096, 256);
    stack.push(std::vector{7.0});
    const auto prefix = stack.cursor();
    CHECK(bound.call(stack) == std::vector<double>{3.0});
    CHECK(stack.depth() == 1);
    CHECK(stack.cursor() == prefix);
    CHECK(bound.call(stack) == std::vector<double>{3.0});
    CHECK(stack.pop<std::vector<double>>() == std::vector<double>{7.0});
}
/// Retains a fixed concrete operand even though its callback is a function template.
struct defaulted_generic {
    /// Returns its exact double operand independently of its defaulted template parameter.
    template <class T = void>
    double operator()(double number) const {
        return number;
    }
};

/// Defaults a non-deduced template operand to double under ordinary invocation.
struct nondeduced_generic {
    /// Returns the exact ordinarily selected operand.
    template <class T = double>
    double operator()(std::type_identity_t<T> number) const {
        return number;
    }
};
TEST_CASE("provably exact generic callbacks retain source operand identities") {
    cel::dyn_segment builder;
    builder.just(cel::value::from(true));
    builder.op1<bool, bool>([](auto flag) { return !flag; });
    builder.op1<bool, bool>([]<class T>(const T& flag) noexcept { return !flag; });
    cel::raw_stack stack;
    CHECK(std::move(builder).finish().prepare<bool>().call(stack));
    cel::dyn_segment forwarded;
    forwarded.just(cel::value::from(2.0));
    forwarded.just(cel::value::from(3.0));
    forwarded.op2<double, double, double>([](auto&&... numbers) { return (numbers + ...); });
    forwarded.op1<double, double>(defaulted_generic{});
    forwarded.op1<double, double>(nondeduced_generic{});
    CHECK(std::move(forwarded).finish().prepare<double>().call(stack) == 5.0);
    cel::dyn_segment mixed;
    mixed.just(cel::value::from(6.0));
    mixed.just(cel::value::from(true));
    mixed.op2<double, double, bool>(cel::with_signature<double(double, bool)>(
        [](double number, auto flag) { return flag ? number : 0.0; }));
    CHECK(std::move(mixed).finish().prepare<double>().call(stack) == 6.0);
    cel::dyn_segment pointer;
    pointer.push_argument(0, cel::value_type::leaf<const client_leaf*>());
    pointer.op1<int, const client_leaf*>([](const auto* source) { return source->number; });
    const client_leaf source{41};
    CHECK(std::move(pointer).finish().prepare<int, const client_leaf*>().call(stack, &source) ==
          41);
}

/// Provides a temporary extent larger than its final scalar result.
struct large_temporary {
    std::array<std::byte, 1024> data{}; ///< Owns a trivially copyable client payload.
};
TEST_CASE("published reservation covers larger temporary canonical extents") {
    cel::dyn_segment builder;
    builder.op0<large_temporary>([] { return large_temporary{}; });
    builder.op1<bool, large_temporary>(
        [](large_temporary source) { return source.data[0] == std::byte{}; });
    auto bound = std::move(builder).finish().prepare<bool>();
    CHECK(bound.requirements().frame_extent == sizeof(large_temporary));
    CHECK(bound.requirements().live_slots == 1);
    cel::raw_stack stack;
    const auto reservation = bound.requirements().for_prefix(stack.cursor(), stack.depth());
    stack.reserve(reservation.bytes.size, reservation.bytes.alignment);
    stack.reserve_depth(reservation.slots);
    CHECK(bound.call(stack));
}

TEST_CASE("published zero-byte peak reserves live slots independently of byte extent") {
    using unit = std::tuple<>;
    cel::dyn_segment builder;
    for (int i = 0; i != 12; ++i)
        builder.op0<unit>([] { return unit{}; });
    builder.op<void, unit, unit, unit, unit, unit, unit, unit, unit, unit, unit, unit, unit>(
        [](auto...) {});
    auto bound = std::move(builder).finish().prepare<unit>();
    CHECK(bound.requirements().frame_extent == 0);
    CHECK(bound.requirements().live_slots == 12);
    cel::raw_stack stack;
    stack.push(unit{});
    stack.push(unit{});
    const auto reservation = bound.requirements().for_prefix(stack.cursor(), stack.depth());
    CHECK(reservation.bytes.size == 0);
    CHECK(reservation.slots == 14);
    stack.reserve(reservation.bytes.size, reservation.bytes.alignment);
    stack.reserve_depth(reservation.slots);
    CHECK(bound.call(stack) == unit{});
    CHECK(stack.depth() == 2);
    CHECK(stack.cursor() == 0);
    CHECK(stack.pop<unit>() == unit{});
    CHECK(stack.pop<unit>() == unit{});
}
