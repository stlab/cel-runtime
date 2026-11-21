// SPDX-License-Identifier: BSL-1.0
#include "storage.hpp"
#include "type_ops.hpp"
#include <cel/runtime/value_type.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cel::detail {

static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559 &&
                  std::numeric_limits<double>::digits == 53,
              "CEL built-in double requires IEEE binary64");
void require_ownership(const value_type& type) {
    if (!type.owns_values())
        throw std::invalid_argument("complete ownership operations unavailable");
}

/// Immutable complete semantic metadata after publication.
struct type_plan : retirement_node {
    // [knowledge] Fields and offsets are canonical and category-appropriate. ownership
    // borrows retained descendants, collapses unary paths, and puts a largest range last.
    // Rebuild these borrowed ranges whenever copied fields are rebound.
    // Keep published fields immutable. Detach every child into the last-owner queue before
    // deletion.
    type_kind kind = type_kind::tuple;        // Identifies the shape.
    layout bytes{0, 1};                       // Describes its canonical representation.
    bool owns = true;                         // Records complete ownership capabilities.
    bool parameter = true;                    // Caches built-in/composite parameter admission.
    std::size_t resources = 0;                // Counts direct ownership carriers.
    std::type_index leaf{typeid(void)};       // Identifies a concrete leaf.
    std::vector<value_type> fields;           // Owns ordered fields or callable parameters.
    std::vector<std::size_t> offsets;         // Owns ordered aggregate offsets.
    std::vector<ownership_slot> ownership;    // Shares sublayouts without expanding descendants.
    std::vector<std::string> names;           // Owns record field names.
    std::string identity;                     // Owns nominal record identity.
    std::optional<value_type> element;        // Retains array/range element identity.
    std::optional<value_type> result;         // Retains a complete callable result.
    range_kind form = range_kind::range_full; // Retains source range form.
    opaque_ownership operations; // Retains trusted callable clone/destruction operations.

    /// Creates unpublished metadata with one initial ownership reference.
    type_plan() : retirement_node(dispose_plan) {}

    /// Copies immutable metadata into a fresh ownership node.
    ///
    /// Complexity: linear in immediate metadata and copied name bytes; new metadata storage.
    type_plan(const type_plan& other) :
        retirement_node(dispose_plan), kind(other.kind), bytes(other.bytes), owns(other.owns),
        parameter(other.parameter), resources(other.resources), leaf(other.leaf),
        fields(other.fields), offsets(other.offsets), ownership(other.ownership),
        names(other.names), identity(other.identity), element(other.element), result(other.result),
        form(other.form), operations(other.operations) {}

    /// Detaches all descriptor edges into queue and deletes the last-owned plan.
    ///
    /// \pre base is an exclusively retiring type_plan.
    /// Complexity: linear in immediate fields and stored names; O(1) auxiliary space.
    static void dispose_plan(retirement_node& base, retirement_queue& queue) noexcept {
        auto& plan = static_cast<type_plan&>(base);
        if (plan.element) type_access::retire(*plan.element, queue);
        if (plan.result) type_access::retire(*plan.result, queue);
        // Clear all owning fields before their containing storage is released.
        for (auto& field : plan.fields)
            type_access::retire(field, queue);
        delete &plan;
    }
};

void type_access::retire(value_type& type, retirement_queue& queue) noexcept {
    type._plan.retire(queue);
}

namespace {
/// Compiles reusable carrier ranges from complete canonical fields.
///
/// \pre plan is unpublished and its fields and offsets agree.
/// Complexity: O(field count) time and added range metadata; descendants are not expanded.
void compile_ownership(type_plan& plan) {
    plan.ownership.clear();
    std::size_t largest = 0;
    std::size_t largest_count = 0;
    // Borrow normalized child ranges instead of copying their transitive slot sequences.
    for (std::size_t i = 0; i != plan.fields.size(); ++i) {
        const auto& field = plan.fields[i];
        const auto count = type_access::resources(field);
        if (count == 0) continue;
        ownership_slot range{&field, plan.offsets[i]};
        const auto children = type_access::ownership_ranges(field);
        if (children.size() == 1) {
            range.type = children.front().type;
            range.offset += children.front().offset;
        }
        if (count > largest_count) {
            largest = plan.ownership.size();
            largest_count = count;
        }
        plan.ownership.push_back(range);
    }
    if (!plan.ownership.empty()) std::swap(plan.ownership[largest], plan.ownership.back());
}

} // namespace
std::size_t plan_pair_hash::operator()(
    const std::pair<const type_plan*, const type_plan*>& pair) const noexcept {
    const auto a = std::hash<const type_plan*>{}(pair.first);
    const auto b = std::hash<const type_plan*>{}(pair.second);
    return a ^ (b + std::size_t{0x9e3779b9} + (a << 6U) + (a >> 2U));
}

value_type type_access::install_callable_ownership(const value_type& signature,
                                                   opaque_ownership operations) {
    if (signature.kind() != type_kind::callable || !operations.clone || !operations.destroy)
        throw std::invalid_argument("callable ownership requires both operations");
    auto plan = std::make_unique<type_plan>(*signature._plan.get());
    plan->operations = operations;
    plan->owns = true;
    return value_type(std::move(plan));
}

value_type type_access::bind_runtime_ownership(const value_type& type, opaque_ownership canonical) {
    assert(canonical.clone && canonical.destroy);
    /// A borrowed position in a source plan's child sequence.
    struct visit {
        // [knowledge] type remains input-owned; next counts already scheduled child edges.
        const value_type* type;
        std::size_t next = 0;
    };
    std::vector<visit> pending{{&type}};
    std::unordered_map<const type_plan*, value_type> bound;
    // Memoized iterative postorder processes each node only after its children.
    while (!pending.empty()) {
        auto& current = pending.back();
        const auto* source = current.type->_plan.get();
        const auto children =
            source->fields.size() + source->element.has_value() + source->result.has_value();
        if (current.next != children) {
            const auto index = current.next++;
            const value_type* child = nullptr;
            if (index < source->fields.size()) {
                child = &source->fields[index];
            } else if (index == source->fields.size() && source->element) {
                child = &*source->element;
            } else {
                assert(source->result);
                child = &*source->result;
            }
            if (!bound.contains(child->_plan.get())) pending.push_back({child});
            continue;
        }
        bool changed = false;
        if (source->kind == type_kind::callable) {
            const auto operations = source->operations;
            if (operations.clone || operations.destroy) {
                if (operations.clone != canonical.clone || operations.destroy != canonical.destroy)
                    throw std::invalid_argument("foreign callable ownership operations");
            } else {
                changed = true;
            }
        }
        const auto transformed = [&](const value_type& child) -> const value_type& {
            return bound.at(child._plan.get());
        };
        const auto child_changed = [&](const value_type& child) {
            return !same_plan(child, transformed(child));
        };
        changed =
            changed || std::any_of(source->fields.begin(), source->fields.end(), child_changed);
        if (source->element) changed = changed || child_changed(*source->element);
        if (source->result) changed = changed || child_changed(*source->result);
        if (!changed) {
            bound.emplace(source, *current.type);
        } else {
            auto plan = std::make_unique<type_plan>(*source);
            std::transform(source->fields.begin(), source->fields.end(), plan->fields.begin(),
                           transformed);
            if (source->element) plan->element = transformed(*source->element);
            if (source->result) plan->result = transformed(*source->result);
            if (plan->kind == type_kind::callable) {
                plan->operations = canonical;
                plan->owns = true;
            } else if (plan->kind == type_kind::array) {
                assert(plan->element);
                plan->owns = plan->element->owns_values();
            } else if (plan->kind == type_kind::tuple || plan->kind == type_kind::record) {
                plan->owns =
                    std::all_of(plan->fields.begin(), plan->fields.end(),
                                [](const value_type& field) { return field.owns_values(); });
                compile_ownership(*plan);
            }
            bound.emplace(source, value_type(std::move(plan)));
        }
        pending.pop_back();
    }
    return bound.at(type._plan.get());
}

opaque_ownership type_access::operations(const value_type& type) noexcept {
    assert(type.kind() == type_kind::callable);
    return type._plan->operations;
}

bool type_access::compatible_operations(const value_type& expected, const value_type& actual) {
    return compare_plans(expected, actual, false);
}

bool type_access::compare_plans(const value_type& expected,
                                const value_type& actual,
                                bool semantic) {
    compatibility_context context;
    return compare_plans(expected, actual, semantic, context);
}

bool type_access::compare_plans(const value_type& expected,
                                const value_type& actual,
                                bool semantic,
                                compatibility_context& context) {
    using pair = std::pair<const type_plan*, const type_plan*>;
    if (expected._plan == actual._plan) return true;
    auto& successful = semantic ? context._semantic : context._operations;
    const pair roots{expected._plan.get(), actual._plan.get()};
    if (successful.contains(roots)) return true;
    context._retained.push_back(expected);
    context._retained.push_back(actual);
    std::vector<pair> pending{{expected._plan.get(), actual._plan.get()}};
    std::unordered_set<pair, plan_pair_hash> visited;
    // Visit each distinct immutable plan pair once, including shared DAG edges.
    while (!pending.empty()) {
        const auto [a, b] = pending.back();
        pending.pop_back();
        if (a == b || successful.contains({a, b}) || !visited.insert({a, b}).second) continue;
        ++context._compared;
        if (a->kind != b->kind) return false;
        if (semantic) {
            if (a->bytes.size != b->bytes.size || a->bytes.alignment != b->bytes.alignment ||
                a->leaf != b->leaf || a->identity != b->identity || a->names != b->names ||
                a->form != b->form || a->offsets != b->offsets ||
                a->element.has_value() != b->element.has_value() ||
                a->result.has_value() != b->result.has_value() ||
                a->fields.size() != b->fields.size())
                return false;
        } else if (a->kind == type_kind::callable) {
            if (a->operations.clone != b->operations.clone ||
                a->operations.destroy != b->operations.destroy)
                return false;
        } else if (a->kind == type_kind::leaf || a->kind == type_kind::range) {
            continue;
        }
        if (a->element && b->element)
            pending.emplace_back(a->element->_plan.get(), b->element->_plan.get());
        if (a->result && b->result)
            pending.emplace_back(a->result->_plan.get(), b->result->_plan.get());
        // Retain only borrowed graph edges; root descriptors own all reachable plans.
        for (std::size_t i = 0; i != a->fields.size(); ++i)
            pending.emplace_back(a->fields[i]._plan.get(), b->fields[i]._plan.get());
    }
    successful.merge(visited);
    return true;
}

std::size_t type_access::resources(const value_type& type) noexcept {
    return type._plan->resources;
}
bool type_access::parameter_allowed(const value_type& type) noexcept {
    return type._plan->parameter;
}

std::span<const ownership_slot> type_access::ownership_ranges(const value_type& type) noexcept {
    return type._plan->ownership;
}

ownership_cursor::ownership_cursor(const value_type& type) noexcept {
    if (type_access::resources(type) == 0) return;
    const auto ranges = type_access::ownership_ranges(type);
    const auto root = ranges.size() == 1 ? ranges.front() : ownership_slot{&type, 0};
    _pending[_size++] = {root.type, root.offset};
}

std::optional<ownership_slot> ownership_cursor::next() noexcept {
    assert(!empty());
    auto& current = _pending[_size - 1];
    const auto ranges = type_access::ownership_ranges(*current.type);
    if (ranges.empty()) {
        const ownership_slot slot{current.type, current.offset};
        --_size;
        return slot;
    }
    const auto range = ranges[current.next++];
    const auto offset = current.offset + range.offset;
    if (current.next == ranges.size()) --_size;
    assert(_size < _pending.size());
    _pending[_size++] = {range.type, offset};
    return std::nullopt;
}

void require_compatible(const value_type& expected, const value_type& actual) {
    compatibility_context context;
    context.require(expected, actual);
}

std::size_t compatibility_context::require(const value_type& expected, const value_type& actual) {
    const auto before = _compared;
    if (!type_access::compare_plans(expected, actual, true, *this) ||
        !type_access::compare_plans(expected, actual, false, *this))
        throw std::invalid_argument("incompatible value shape or opaque ownership operations");
    return _compared - before;
}

namespace {
/// Extends a plan's canonical layout and complete aggregate capabilities with source-order fields.
///
/// \pre plan has a valid canonical prefix.
/// Throws std::overflow_error if byte extent arithmetic overflows.
/// Also throws std::overflow_error if the ownership-carrier count overflows.
/// Failure may leave a partially extended unpublished plan.
/// Complexity: O(field count) amortized time and added offset result storage.
void canonical_layout(type_plan& plan, std::span<const value_type> fields) {
    // Accumulate source-order layouts and retain canonical field offsets.
    for (const auto& field : fields) {
        const auto bytes = field.byte_layout();
        const auto offset = align_offset(plan.bytes.size, bytes.alignment);
        if (bytes.size > std::numeric_limits<std::size_t>::max() - offset)
            throw std::overflow_error("aggregate layout overflow");
        plan.offsets.push_back(offset);
        plan.bytes.size = offset + bytes.size;
        plan.bytes.alignment = std::max(plan.bytes.alignment, bytes.alignment);
        plan.owns = plan.owns && field.owns_values();
        plan.parameter = plan.parameter && type_access::parameter_allowed(field);
        const auto count = type_access::resources(field);
        if (count > std::numeric_limits<std::size_t>::max() - plan.resources)
            throw std::overflow_error("aggregate ownership count overflow");
        plan.resources += count;
    }
    plan.bytes.size = align_offset(plan.bytes.size, plan.bytes.alignment);
}

/// Returns whether the type satisfies built-in/composite callable parameter admission.
bool parameter_allowed(const value_type& type) { return type_access::parameter_allowed(type); }

} // namespace
} // namespace cel::detail

namespace cel {

value_type::value_type(std::unique_ptr<detail::type_plan> plan) : _plan(plan.release()) {}
value_type::value_type() : value_type(tuple({})) {}

value_type::~value_type() = default;
value_type& value_type::operator=(const value_type& other) noexcept {
    _plan = other._plan;
    return *this;
}

value_type value_type::make_leaf(std::type_index identity, layout bytes) {
    (void)detail::align_offset(bytes.size, bytes.alignment);
    auto plan = std::make_unique<detail::type_plan>();
    plan->kind = type_kind::leaf;
    plan->bytes = bytes;
    plan->leaf = identity;
    plan->parameter = identity == typeid(bool) || identity == typeid(double);
    return value_type(std::move(plan));
}

value_type value_type::array(value_type element) {
    auto plan = std::make_unique<detail::type_plan>();
    plan->kind = type_kind::array;
    plan->bytes = {sizeof(void*), alignof(void*)};
    plan->owns = element.owns_values();
    plan->parameter = element._plan->parameter;
    plan->resources = 1;
    plan->element = std::move(element);
    return value_type(std::move(plan));
}

value_type value_type::tuple(std::span<const value_type> fields) {
    auto plan = std::make_unique<detail::type_plan>();
    plan->fields.assign(fields.begin(), fields.end());
    detail::canonical_layout(*plan, fields);
    detail::compile_ownership(*plan);
    return value_type(std::move(plan));
}

value_type value_type::record(std::string_view identity, std::span<const record_field> fields) {
    if (identity.empty()) throw std::invalid_argument("record identity is empty");
    auto plan = std::make_unique<detail::type_plan>();
    plan->kind = type_kind::record;
    plan->identity = identity;
    std::unordered_set<std::string_view> names;
    // Validate and retain source-order names and complete component types.
    for (const auto& field : fields) {
        if (field.name.empty() || !names.insert(field.name).second)
            throw std::invalid_argument("record field name is empty or duplicated");
        plan->names.push_back(field.name);
        plan->fields.push_back(field.type);
    }
    detail::canonical_layout(*plan, plan->fields);
    detail::compile_ownership(*plan);
    return value_type(std::move(plan));
}

value_type value_type::range(range_kind form, value_type endpoint) {
    if (endpoint.kind() != type_kind::leaf)
        throw std::invalid_argument("range endpoint must be a scalar leaf");
    std::size_t count = 0;
    switch (form) {
        case range_kind::range:
        case range_kind::range_inclusive:
            count = 2;
            break;
        case range_kind::range_from:
        case range_kind::range_to:
        case range_kind::range_to_inclusive:
            count = 1;
            break;
        case range_kind::range_full:
            break;
        default:
            throw std::invalid_argument("invalid range form");
    }
    auto plan = std::make_unique<detail::type_plan>();
    plan->kind = type_kind::range;
    plan->form = form;
    plan->element = endpoint;
    const std::array endpoints{endpoint, endpoint};
    detail::canonical_layout(*plan, std::span(endpoints).first(count));
    plan->parameter = endpoint._plan->parameter;
    return value_type(std::move(plan));
}

value_type value_type::callable(std::span<const value_type> parameters, value_type result) {
    if (!std::all_of(parameters.begin(), parameters.end(), detail::parameter_allowed))
        throw std::invalid_argument("callable parameters require built-in composite values");
    auto plan = std::make_unique<detail::type_plan>();
    plan->kind = type_kind::callable;
    plan->bytes = {sizeof(void*), alignof(void*)};
    plan->owns = false;
    plan->parameter = false;
    plan->resources = 1;
    plan->fields.assign(parameters.begin(), parameters.end());
    plan->result = std::move(result);
    return value_type(std::move(plan));
}

bool value_type::same_shape(const value_type& other) const {
    return detail::type_access::compare_plans(*this, other, true);
}

bool value_type::compatible_with(const value_type& other) const {
    detail::compatibility_context context;
    return detail::type_access::compare_plans(*this, other, true, context) &&
           detail::type_access::compare_plans(*this, other, false, context);
}

type_kind value_type::kind() const noexcept { return _plan->kind; }
layout value_type::byte_layout() const noexcept { return _plan->bytes; }
bool value_type::owns_values() const noexcept { return _plan->owns; }

std::type_index value_type::leaf_identity() const {
    if (kind() != type_kind::leaf) throw std::invalid_argument("not a concrete leaf");
    return _plan->leaf;
}
const value_type& value_type::element() const {
    if (!_plan->element) throw std::invalid_argument("not an array or range");
    return *_plan->element;
}
std::span<const value_type> value_type::fields() const {
    if (kind() != type_kind::tuple && kind() != type_kind::record)
        throw std::invalid_argument("not a tuple or record");
    return _plan->fields;
}
std::span<const std::size_t> value_type::offsets() const {
    if (kind() != type_kind::tuple && kind() != type_kind::record && kind() != type_kind::range)
        throw std::invalid_argument("not a fixed aggregate");
    return _plan->offsets;
}
std::string_view value_type::record_identity() const {
    if (kind() != type_kind::record) throw std::invalid_argument("not a record");
    return _plan->identity;
}
std::span<const std::string> value_type::field_names() const {
    if (kind() != type_kind::record) throw std::invalid_argument("not a record");
    return _plan->names;
}
range_kind value_type::form() const {
    if (kind() != type_kind::range) throw std::invalid_argument("not a range");
    return _plan->form;
}
std::span<const value_type> value_type::parameters() const {
    if (kind() != type_kind::callable) throw std::invalid_argument("not a callable");
    return _plan->fields;
}
const value_type& value_type::result() const {
    if (!_plan->result) throw std::invalid_argument("not a callable");
    return *_plan->result;
}

} // namespace cel

#include <cpp-library/testing.hpp>
#if CPP_LIBRARY_TESTING()
#include "value_ops.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cel/runtime/value.hpp>
#include <doctest/doctest.h>
#include <latch>
#include <limits>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
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
} // namespace

#endif
