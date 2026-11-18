// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_RECORD_ADAPTER_HPP
#define CEL_RUNTIME_RECORD_ADAPTER_HPP

#include <cel/runtime/value_type.hpp>

namespace cel {
class value;

/// Adapts one client nominal record type to and from canonical CEL record values.
///
/// Specialize this template for one client type T to expose it as a nominal record.
/// type() must return a nominal record descriptor. to_value() must return a value with
/// exactly that descriptor. from_value() should validate complete descriptor identity
/// before decoding and throw std::invalid_argument when mismatched.
///
/// Example:
/// @code
/// struct point {
///     double x;
///     double y;
/// };
///
/// template <>
/// struct cel::record_adapter<point> {
///     static cel::value_type type() {
///         static const auto descriptor = cel::value_type::record(
///             "sample.point",
///             std::array{cel::record_field{"x", cel::value_type::leaf<double>()},
///                        cel::record_field{"y", cel::value_type::leaf<double>()}});
///         return descriptor;
///     }
///     static cel::value to_value(const point& source);
///     static point from_value(const cel::value& source);
/// };
/// @endcode
template <class T>
struct record_adapter {};

} // namespace cel

#endif
