// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_HPP
#define CEL_RUNTIME_HPP

/// \file
/// Exposes the installed C++20 runtime's checked values, builders, and execution surfaces.
/// Include this header and link to cel::runtime; private canonical storage stays internal.
/// \see cel::dyn_segment and cel::raw_stack for cached execution and reusable storage.

#include <cel/runtime/callable.hpp>
#include <cel/runtime/dyn_segment.hpp>
#include <cel/runtime/layout.hpp>
#include <cel/runtime/raw_segment.hpp>
#include <cel/runtime/raw_stack.hpp>
#include <cel/runtime/segment.hpp>
#include <cel/runtime/value.hpp>
#include <cel/runtime/value_type.hpp>

#endif
