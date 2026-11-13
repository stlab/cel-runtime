// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_HPP
#define CEL_RUNTIME_HPP

/// \file
/// Provides values, program builders, and reusable execution for C++20 runtime clients.
///
/// Include this header and link to cel::runtime to build and evaluate programs.
/// This library does not parse CEL source or register built-in operations.
/// \see cel::dyn_segment and cel::raw_stack for cached execution and reusable storage.
///
/// Build a checked program and reuse its typed binding:
/// @code
/// #include <cel/runtime.hpp>
///
/// cel::dyn_segment builder;
/// builder.push_argument(0, cel::native_type<double>());
/// builder.op1<double, double>([](double n) { return n * 2.0; });
/// auto program = std::move(builder).finish();
/// auto bound = program.prepare<double, double>();
/// cel::raw_stack stack;
/// const auto needed =
///     bound.requirements().for_prefix(stack.cursor(), stack.depth());
/// stack.reserve(needed.bytes.size, needed.bytes.alignment);
/// stack.reserve_depth(needed.slots);
/// auto result = bound.call(stack, 3.0); // 6.0; stack is empty again.
/// @endcode

// [knowledge] Keep canonical storage private; public adapters do not expose its representation.

#include <cel/runtime/callable.hpp>
#include <cel/runtime/dyn_segment.hpp>
#include <cel/runtime/layout.hpp>
#include <cel/runtime/raw_stack.hpp>
#include <cel/runtime/value.hpp>
#include <cel/runtime/value_type.hpp>

#endif
