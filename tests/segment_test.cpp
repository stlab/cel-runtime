// SPDX-License-Identifier: BSL-1.0
#include "type_ops.hpp"
#include "value_ops.hpp"
#include <cel/runtime/dyn_segment.hpp>
#include <cel/runtime/segment.hpp>

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <array>
#include <doctest/doctest.h>
#include <limits>
#include <memory>
#include <tuple>
#include <vector>

/// Counts live synthetic callable-carrier owners without implementing callable bodies.
int live_carriers = 0;
/// Clones an independently owned integer through the foundation's opaque ownership contract.
void clone_carrier(const std::byte* source, std::byte* destination) {
    int* pointer = nullptr;
    std::memcpy(static_cast<void*>(&pointer), source, sizeof(pointer));
    auto owner = std::make_unique<int>(*pointer);
    pointer = owner.release();
    std::memcpy(destination, static_cast<const void*>(&pointer), sizeof(pointer));
    ++live_carriers;
}
/// Retires one initialized synthetic carrier.
void destroy_carrier(std::byte* bytes) noexcept {
    int* pointer = nullptr;
    std::memcpy(static_cast<void*>(&pointer), bytes, sizeof(pointer));
    delete pointer;
    --live_carriers;
}
/// Supplies an incompatible ownership pair with the same semantic signature.
void destroy_other_carrier(std::byte* bytes) noexcept { destroy_carrier(bytes); }

/// Detects whether a final static stack admits standalone finalization.
template <class T>
concept finishable = requires(T builder) {
    std::move(builder).finish();
};

/// Exercises root admission independently of callable-parameter restrictions.
struct alignas(256) client_leaf {
    int number; ///< Retains a client-defined scalar.
};

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

TEST_CASE("recursive preparation mismatch preserves nominal and leaf identity") {
    using input = std::tuple<std::vector<double>, bool>;
    cel::dyn_segment builder;
    builder.push_argument(0, cel::detail::concrete_adapter<input>::type());
    auto program = std::move(builder).finish();
    CHECK_THROWS_AS((program.prepare<input, std::tuple<std::vector<bool>, bool>>()),
                    std::invalid_argument);
    CHECK_THROWS_AS((program.prepare<std::tuple<std::vector<double>, double>, input>()),
                    std::invalid_argument);
    cel::raw_stack stack;
    const std::array wrong{cel::value::from(std::tuple{std::vector{true}, true})};
    CHECK_THROWS_AS(program.execute(stack, wrong), std::invalid_argument);
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

TEST_CASE("typed composite calls borrow inputs and return disjoint nested results") {
    using input = std::tuple<std::vector<std::vector<double>>, bool>;
    cel::dyn_segment builder;
    builder.push_argument(0, cel::detail::concrete_adapter<input>::type());
    auto prepared = std::move(builder).finish().prepare<input, input>();
    cel::raw_stack stack;
    input source{{{1.0, 2.0}}, true};
    auto first = prepared.call(stack, source);
    auto second = prepared.call(stack, source);
    std::get<0>(first)[0][0] = 7.0;
    CHECK(std::get<0>(source)[0][0] == 1.0);
    CHECK(std::get<0>(second)[0][0] == 1.0);
    std::get<0>(source)[0][1] = 9.0;
    CHECK(std::get<0>(second)[0][1] == 2.0);
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

TEST_CASE("dynamic static and selected callbacks copy nonmovable lvalues independently") {
    copy_only_factor callback(3.0);
    cel::dyn_segment dynamic;
    dynamic.just(cel::value::from(2.0));
    dynamic.op1<double, double>(callback);
    cel::segment<cel::type_list<double>> typed;
    auto scaled = std::move(typed).op1<double>(callback);
    auto selected = cel::with_signature<double(double)>(callback);
    cel::dyn_segment explicit_signature;
    explicit_signature.just(cel::value::from(4.0));
    explicit_signature.op1<double, double>(selected);
    callback.factor = 99.0;
    cel::raw_stack stack;
    CHECK(std::move(dynamic).finish().prepare<double>().call(stack) == 6.0);
    CHECK(std::move(scaled).finish().prepare<double, double>().call(stack, 5.0) == 15.0);
    CHECK(std::move(explicit_signature).finish().prepare<double>().call(stack) == 12.0);
}

TEST_CASE("typed zero-sized unit arrays retain nested logical lengths") {
    using units = std::vector<std::vector<std::tuple<>>>;
    cel::dyn_segment builder;
    builder.push_argument(0, cel::detail::concrete_adapter<units>::type());
    auto prepared = std::move(builder).finish().prepare<units, units>();
    const units input{std::vector<std::tuple<>>(3), {}, std::vector<std::tuple<>>(1)};
    cel::raw_stack stack;
    auto result = prepared.call(stack, input);
    REQUIRE(result.size() == 3);
    CHECK(result[0].size() == 3);
    CHECK(result[1].empty());
    CHECK(result[2].size() == 1);
    result[0].clear();
    CHECK(input[0].size() == 3);
    CHECK(prepared.call(stack, input)[0].size() == 3);
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

TEST_CASE("static initial operands retain source order and type flow") {
    cel::segment<cel::type_list<double, bool, double>> builder;
    auto operation = std::move(builder).op3<double>(
        [](double a, bool b, double c) { return b ? a - c : c - a; });
    auto prepared = std::move(operation).finish().prepare<double, double, bool, double>();
    cel::raw_stack stack;
    CHECK(prepared.call(stack, 10.0, true, 2.0) == 8.0);
    cel::segment<cel::type_list<double>> identity;
    CHECK(std::move(identity).finish().prepare<double, double>().call(stack, 6.0) == 6.0);
    cel::segment<cel::type_list<>> empty;
    CHECK(std::move(empty).finish().prepare<std::tuple<>>().call(stack) == std::tuple<>{});
}

TEST_CASE("static pushes unary binary arbitrary arity and void source unit") {
    cel::segment<cel::type_list<double>> builder;
    auto unary = std::move(builder).op1<bool>([](double n) { return n > 0.0; });
    auto pushed = std::move(unary).op0<double>([] { return 3.0; });
    auto binary = std::move(pushed).op2<double>([](bool b, double n) { return b ? n : -n; });
    auto consumed = std::move(binary).op<void, double>([](double) {});
    cel::raw_stack stack;
    CHECK(std::move(consumed).finish().prepare<std::tuple<>, double>().call(stack, 2.0) ==
          std::tuple<>{});
}

TEST_CASE("finalization rejects silent leftover operand discard") {
    cel::dyn_segment builder;
    builder.just(cel::value::from(true));
    builder.just(cel::value::from(2.0));
    CHECK_THROWS_AS(std::move(builder).finish(), std::invalid_argument);
}

TEST_CASE("runtime-bound owning output has a public retirement boundary") {
    cel::dyn_segment builder;
    builder.op0<std::vector<std::vector<double>>>(
        [] { return std::vector<std::vector<double>>{{2.0}, {3.0}}; });
    auto program = std::move(builder).finish();
    auto prepared = program.prepare_raw({}, program.result_type());
    const auto layout = prepared.result_type().byte_layout();
    auto* destination =
        static_cast<std::byte*>(::operator new(layout.size, std::align_val_t(layout.alignment)));
    cel::raw_stack stack;
    prepared.call_into(stack, {}, destination);
    auto first = cel::borrowed_value{prepared.result_type(), destination}
                     .as<std::vector<std::vector<double>>>();
    prepared.destroy_result(destination);
    first[0][0] = 8.0;
    prepared.call_into(stack, {}, destination);
    CHECK(cel::borrowed_value{prepared.result_type(), destination}
              .as<std::vector<std::vector<double>>>()[0][0] == 2.0);
    prepared.destroy_result(destination);
    ::operator delete(destination, std::align_val_t(layout.alignment));
    CHECK(stack.depth() == 0);
}

TEST_CASE("runtime-bound exact record identity and shared deep metadata") {
    const std::array fields{cel::record_field{"flag", cel::value_type::leaf<bool>()}};
    auto type = cel::value_type::record("named", fields);
    cel::dyn_segment builder;
    builder.push_argument(0, type);
    auto program = std::move(builder).finish();
    const std::array wrong{cel::value_type::record("other", fields)};
    CHECK_THROWS_AS(program.prepare_raw(wrong, type), std::invalid_argument);
    const std::array arguments{type};
    auto prepared = program.prepare_raw(arguments, type);
    const std::array contents{cel::value::from(true)};
    auto source = cel::value::record(type, contents);
    const std::array views{cel::borrowed_value{type, cel::detail::value_access::data(source)}};
    alignas(bool) std::array<std::byte, sizeof(bool)> result{};
    cel::raw_stack stack;
    prepared.call_into(stack, views, result.data());
    CHECK(cel::detail::value_access::copy(type, result.data()).field("flag").as<bool>());
    prepared.destroy_result(result.data());
    auto deep = cel::value_type::leaf<bool>();
    auto same = cel::value_type::leaf<bool>();
    for (int i = 0; i != 32; ++i) {
        const std::array left{deep, deep};
        const std::array right{same, same};
        deep = cel::value_type::tuple(left);
        same = cel::value_type::tuple(right);
    }
    cel::dyn_segment shared;
    shared.push_argument(0, deep);
    auto shared_program = std::move(shared).finish();
    const std::array shared_arguments{same};
    CHECK_NOTHROW(shared_program.prepare_raw(shared_arguments, same));
}

TEST_CASE("throwing callbacks preserve caller prefix and reusable raw destination") {
    bool fail = true;
    cel::dyn_segment builder;
    builder.push_argument(0, cel::value_type::leaf<double>());
    builder.op1<double, double>([&fail](double n) {
        if (fail) throw std::runtime_error("callback");
        return n + 1.0;
    });
    auto program = std::move(builder).finish();
    const std::array types{cel::value_type::leaf<double>()};
    auto prepared = program.prepare_raw(types, types[0]);
    cel::raw_stack stack;
    stack.push(std::vector{7.0});
    const auto cursor = stack.cursor();
    double input = 2.0;
    const std::array views{
        cel::borrowed_value{types[0], reinterpret_cast<const std::byte*>(&input)}};
    alignas(double) std::array<std::byte, sizeof(double)> result{};
    result.fill(std::byte{0x5a});
    CHECK_THROWS_AS(prepared.call_into(stack, views, result.data()), std::runtime_error);
    CHECK(result[0] == std::byte{0x5a});
    CHECK(stack.depth() == 1);
    CHECK(stack.cursor() == cursor);
    fail = false;
    prepared.call_into(stack, views, result.data());
    CHECK(cel::borrowed_value{types[0], result.data()}.as<double>() == 3.0);
    prepared.destroy_result(result.data());
    CHECK(stack.pop<std::vector<double>>() == std::vector<double>{7.0});
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

TEST_CASE("reserved typed and runtime-bound scalar calls reuse the stack") {
    cel::dyn_segment builder;
    builder.push_argument(0, cel::value_type::leaf<double>());
    builder.just(cel::value::from(3.0));
    builder.op2<double, double, double>([](double a, double b) { return a - b; });
    auto program = std::move(builder).finish();
    auto typed = program.prepare<double, double>();
    const std::array types{cel::value_type::leaf<double>()};
    auto raw = program.prepare_raw(types, types[0]);
    cel::raw_stack stack;
    const auto reservation = typed.requirements().for_prefix(stack.cursor(), stack.depth());
    stack.reserve(reservation.bytes.size, reservation.bytes.alignment);
    stack.reserve_depth(reservation.slots);
    CHECK(typed.call(stack, 5.0) == 2.0);
    double input = 8.0;
    const std::array views{
        cel::borrowed_value{types[0], reinterpret_cast<const std::byte*>(&input)}};
    alignas(double) std::array<std::byte, sizeof(double)> bytes{};
    raw.call_into(stack, views, bytes.data());
    raw.destroy_result(bytes.data());
    double typed_total = 0.0;
    double raw_total = 0.0;
    for (int i = 0; i != 1000; ++i) {
        typed_total += typed.call(stack, input);
        raw.call_into(stack, views, bytes.data());
        raw_total += cel::detail::concrete_adapter<double>::read(types[0], bytes.data());
        raw.destroy_result(bytes.data());
    }
    CHECK(typed_total == 5000.0);
    CHECK(raw_total == 5000.0);
    CHECK(stack.depth() == 0);
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

TEST_CASE("raw calls preserve composite arguments and caller prefix") {
    int effects = 0;
    cel::dyn_segment builder;
    const auto type = cel::detail::concrete_adapter<std::vector<double>>::type();
    builder.push_argument(0, type);
    builder.push_argument(0, type);
    builder.op2<double, std::vector<double>, std::vector<double>>(
        [&effects](std::vector<double> a, std::vector<double> b) {
            ++effects;
            return a[0] + b[0];
        });
    auto program = std::move(builder).finish();
    const std::array types{type};
    auto bound = program.prepare_raw(types, cel::value_type::leaf<double>());
    const auto owner = cel::value::from(std::vector{2.0});
    const std::array views{cel::borrowed_value{type, cel::detail::value_access::data(owner)}};
    cel::raw_stack stack;
    stack.reserve(4096, 256);
    stack.push(std::vector{9.0});
    const auto prefix = stack.cursor();
    alignas(double) std::array<std::byte, sizeof(double)> bytes{};
    CHECK(effects == 0);
    bound.call_into(stack, views, bytes.data());
    CHECK(cel::borrowed_value{bound.result_type(), bytes.data()}.as<double>() == 4.0);
    bound.destroy_result(bytes.data());
    CHECK(effects == 1);
    CHECK(owner.as<std::vector<double>>() == std::vector{2.0});
    CHECK(stack.cursor() == prefix);
    CHECK(stack.depth() == 1);
    CHECK(stack.pop<std::vector<double>>() == std::vector<double>{9.0});
}

TEST_CASE("frozen raw callable results bind complete ownership and retire independently") {
    const auto signature = cel::value_type::callable({}, cel::value_type::leaf<client_leaf>());
    const auto type = cel::detail::type_access::install_callable_ownership(
        signature, {clone_carrier, destroy_carrier});
    const auto other = cel::detail::type_access::install_callable_ownership(
        signature, {clone_carrier, destroy_other_carrier});
    const auto composite = cel::value_type::array(type);
    const auto incompatible = cel::value_type::array(other);
    int number = 42;
    int* pointer = &number;
    alignas(void*) std::array<std::byte, sizeof(pointer)> bytes{};
    std::memcpy(bytes.data(), static_cast<const void*>(&pointer), sizeof(pointer));
    CHECK(live_carriers == 0);
    {
        const auto owner = cel::detail::value_access::copy(type, bytes.data());
        const std::array elements{owner};
        auto literal = cel::value::array(type, elements);
        cel::dyn_segment builder;
        builder.just(std::move(literal));
        auto program = std::move(builder).finish();
        CHECK_THROWS_AS(program.prepare_raw({}, incompatible), std::invalid_argument);
        CHECK_THROWS_AS(program.prepare_raw({}, cel::value_type::array(signature)),
                        std::invalid_argument);
        auto bound = program.prepare_raw({}, composite);
        const auto layout = composite.byte_layout();
        auto* result = static_cast<std::byte*>(
            ::operator new(layout.size, std::align_val_t(layout.alignment)));
        cel::raw_stack stack;
        const auto baseline = live_carriers;
        bound.call_into(stack, {}, result);
        CHECK(live_carriers == baseline + 1);
        bound.destroy_result(result);
        CHECK(live_carriers == baseline);
        bound.call_into(stack, {}, result);
        CHECK(live_carriers == baseline + 1);
        bound.destroy_result(result);
        CHECK(live_carriers == baseline);
        ::operator delete(result, std::align_val_t(layout.alignment));
    }
    CHECK(live_carriers == 0);
}

TEST_CASE("checked callable read pairs and range identities reject before effects") {
    const auto signature = cel::value_type::callable({}, cel::value_type::leaf<bool>());
    const auto type = cel::detail::type_access::install_callable_ownership(
        signature, {clone_carrier, destroy_carrier});
    const auto other = cel::detail::type_access::install_callable_ownership(
        signature, {clone_carrier, destroy_other_carrier});
    int effects = 0;
    cel::dyn_segment builder;
    builder.op0<void>([&effects] { ++effects; });
    builder.push_argument(0, type);
    CHECK_THROWS_AS(builder.push_argument(0, other), std::invalid_argument);
    auto program = std::move(builder).finish();
    const std::array wrong_types{other};
    CHECK_THROWS_AS(program.prepare_raw(wrong_types, type), std::invalid_argument);
    int number = 3;
    int* pointer = &number;
    alignas(void*) std::array<std::byte, sizeof(pointer)> bytes{};
    std::memcpy(bytes.data(), static_cast<const void*>(&pointer), sizeof(pointer));
    const std::array wrong_values{cel::detail::value_access::copy(other, bytes.data())};
    cel::raw_stack stack;
    CHECK_THROWS_AS(program.execute(stack, wrong_values), std::invalid_argument);
    CHECK(effects == 0);
    const auto range =
        cel::value_type::range(cel::range_kind::range_full, cel::value_type::leaf<double>());
    cel::dyn_segment ranges;
    ranges.push_argument(0, range);
    auto range_program = std::move(ranges).finish();
    const std::array wrong_range{
        cel::value_type::range(cel::range_kind::range_full, cel::value_type::leaf<bool>())};
    CHECK_THROWS_AS(range_program.prepare_raw(wrong_range, range), std::invalid_argument);
    const std::array right_range{range};
    auto bound = range_program.prepare_raw(right_range, range);
    const std::array range_input{cel::borrowed_value{range, nullptr}};
    CHECK_NOTHROW(bound.call_into(stack, range_input, nullptr));
    bound.destroy_result(nullptr);
    CHECK(stack.depth() == 0);
}

TEST_CASE("zero-byte non-unit raw arguments retain complete recursive identity") {
    using zero = std::tuple<std::tuple<>, std::tuple<>>;
    const auto type = cel::detail::concrete_adapter<zero>::type();
    cel::dyn_segment builder;
    builder.push_argument(0, type);
    auto program = std::move(builder).finish();
    CHECK_THROWS_AS((program.prepare<std::tuple<>, zero>()), std::invalid_argument);
    const std::array types{type};
    auto bound = program.prepare_raw(types, type);
    const std::array inputs{cel::borrowed_value{type, nullptr}};
    cel::raw_stack stack;
    CHECK_NOTHROW(bound.call_into(stack, inputs, nullptr));
    bound.destroy_result(nullptr);
    CHECK(program.prepare<zero, zero>().call(stack, zero{}) == zero{});
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

TEST_CASE("static finalization and template ascription cannot erase live operands") {
    using two = cel::segment<cel::type_list<double, bool>>;
    using one = cel::segment<cel::type_list<double, bool>, cel::type_list<bool>>;
    static_assert(!finishable<two>);
    static_assert(!std::default_initializable<one>);
    static_assert(!std::constructible_from<one, two>);
    cel::segment<cel::type_list<double, double, bool, double>> builder;
    auto combined = std::move(builder).op<double, double, double, bool, double>(
        [](double a, double b, bool flag, double c) { return flag ? a + b - c : 0.0; });
    cel::raw_stack stack;
    CHECK(std::move(combined).finish().prepare<double, double, double, bool, double>().call(
              stack, 10.0, 4.0, true, 2.0) == 12.0);
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

/// Supplies named overloads that require an exact explicit signature selection.
struct overloaded_callback {
    /// Doubles one exact operand.
    double operator()(double number) const { return number * 2.0; }
    /// Adds two exact operands.
    double operator()(double a, double b) const { return a + b; }
};

/// Exercises invocation through the required const lvalue callback boundary.
struct lvalue_callback {
    /// Increments its exact operand through a const lvalue.
    double operator()(double number) const& { return number + 1.0; }
};

/// Exercises nonthrowing invocation through the same const lvalue boundary.
struct lvalue_noexcept_callback {
    /// Increments its exact operand without throwing.
    double operator()(double number) const& noexcept { return number + 2.0; }
};

/// Supplies an introspection-ineligible invocation through an exact function-pointer conversion.
struct pointer_callback {
    using function = double (*)(double); ///< Describes its explicit concrete invocation.

    /// Doubles one exact operand.
    static double twice(double number) { return number * 2.0; }
    /// Returns its exact function pointer.
    operator function() const noexcept { return twice; }
};

/// Detects exact explicit member selection without hard introspection errors.
template <class Signature, class F>
concept signature_selectable = requires(F callback) {
    cel::with_signature<Signature>(std::move(callback));
};

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

TEST_CASE("exact callback selection rejects ambiguous conversions and accepts const lvalues") {
    static_assert(!signature_selectable<double(bool), overloaded_callback>);
    static_assert(!cel::detail::exact_callback<pointer_callback, double, double>());
    static_assert(!signature_selectable<double(double), pointer_callback>);
    cel::dyn_segment builder;
    builder.just(cel::value::from(3.0));
    builder.op1<double, double>(cel::with_signature<double(double)>(overloaded_callback{}));
    builder.op1<double, double>(lvalue_callback{});
    builder.op1<double, double>(lvalue_noexcept_callback{});
    cel::raw_stack stack;
    CHECK(std::move(builder).finish().prepare<double>().call(stack) == 9.0);
    cel::dyn_segment binary;
    binary.just(cel::value::from(2.0));
    binary.just(cel::value::from(5.0));
    binary.op2<double, double, double>(
        cel::with_signature<double(double, double)>(overloaded_callback{}));
    CHECK(std::move(binary).finish().prepare<double>().call(stack) == 7.0);
    cel::dyn_segment pointer;
    pointer.just(cel::value::from(4.0));
    pointer.op1<double, double>(static_cast<pointer_callback::function>(pointer_callback{}));
    pointer.op1<double, double>(pointer_callback::twice);
    CHECK(std::move(pointer).finish().prepare<double>().call(stack) == 16.0);
    cel::dyn_segment qualified;
    qualified.just(cel::value::from(3.0));
    qualified.op1<double, double>(cel::with_signature<double(double)>(lvalue_noexcept_callback{}));
    CHECK(std::move(qualified).finish().prepare<double>().call(stack) == 5.0);
}

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

TEST_CASE("published reservations cover hidden intermediate alignment and caller padding") {
    cel::dyn_segment builder;
    builder.op0<double>([] { return 2.0; });
    builder.op0<client_leaf>([] { return client_leaf{7}; });
    builder.op1<double, client_leaf>(
        [](const client_leaf& source) { return double(source.number); });
    builder.op2<double, double, double>([](double a, double b) { return a + b; });
    auto program = std::move(builder).finish();
    CHECK(program.result_type().byte_layout().alignment == alignof(double));
    const auto& requirements = program.requirements();
    CHECK(requirements.alignment == alignof(client_leaf));
    CHECK(requirements.frame_extent == 512);
    CHECK(requirements.live_slots == 2);
    auto typed = program.prepare<double>();
    auto raw = program.prepare_raw({}, program.result_type());
    CHECK(&typed.requirements() == &program.requirements());
    CHECK(&raw.requirements() == &program.requirements());
    cel::raw_stack stack;
    stack.push(true);
    const auto prefix_cursor = stack.cursor();
    const auto prefix_depth = stack.depth();
    const auto reservation = typed.requirements().for_prefix(prefix_cursor, prefix_depth);
    CHECK(reservation.bytes.size == 768);
    CHECK(reservation.slots == 3);
    stack.reserve(reservation.bytes.size, reservation.bytes.alignment);
    stack.reserve_depth(reservation.slots);
    alignas(double) std::array<std::byte, sizeof(double)> bytes{};
    const auto result = typed.call(stack);
    raw.call_into(stack, {}, bytes.data());
    raw.destroy_result(bytes.data());
    CHECK(result == 9.0);
    CHECK(stack.cursor() == prefix_cursor);
    CHECK(stack.depth() == prefix_depth);
    CHECK(stack.pop<bool>());
}

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

TEST_CASE("published reservations check prefix arithmetic and reject construction overflow") {
    cel::dyn_segment builder;
    builder.op0<double>([] { return 2.0; });
    auto program = std::move(builder).finish();
    const auto maximum = std::numeric_limits<std::size_t>::max();
    CHECK_THROWS_AS(program.requirements().for_prefix(maximum, 0), std::overflow_error);
    CHECK_THROWS_AS(program.requirements().for_prefix(0, maximum), std::overflow_error);
    auto malformed = program.requirements();
    malformed.alignment = 3;
    CHECK_THROWS_AS(malformed.for_prefix(0, 0), std::invalid_argument);
    auto huge = cel::value_type::leaf<bool>();
    for (int i = 0; i != std::numeric_limits<std::size_t>::digits - 1; ++i) {
        const std::array fields{huge, huge};
        huge = cel::value_type::tuple(fields);
    }
    cel::dyn_segment wide;
    wide.push_argument(0, huge);
    CHECK_THROWS_AS(wide.push_argument(1, huge), std::overflow_error);
    auto frozen = std::move(wide).finish();
    CHECK(frozen.argument_reads().size() == 1);
    CHECK(frozen.requirements().frame_extent == huge.byte_layout().size);
    CHECK(frozen.requirements().live_slots == 1);
}
