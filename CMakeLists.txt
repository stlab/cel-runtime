cmake_minimum_required(VERSION 3.24)

include(cmake/CPM.cmake)

# Fetch cpp-library before project()
CPMAddPackage("gh:stlab/cpp-library@7.0.0")
include(${cpp-library_SOURCE_DIR}/cpp-library.cmake)

# Enable dependency tracking before project()
cpp_library_enable_dependency_tracking()

# Now declare project
project(runtime LANGUAGES CXX)

include(CTest)

# Setup library
cpp_library_setup(
    DESCRIPTION "Checked CEL programs, owned values, and reusable execution stacks"
    NAMESPACE cel
    HEADERS runtime.hpp runtime/layout.hpp runtime/value_type.hpp runtime/value.hpp runtime/record_adapter.hpp runtime/raw_stack.hpp runtime/dyn_segment.hpp runtime/callable.hpp runtime/detail/retirement.hpp runtime/detail/frame.hpp runtime/detail/raw_segment.hpp
    SOURCES storage.cpp value_type.cpp value.cpp raw_stack.cpp raw_segment.cpp dyn_segment.cpp callable.cpp
    TESTS storage_test.cpp value_test.cpp resource_test.cpp raw_execution_test.cpp segment_test.cpp composite_execution_test.cpp callable_test.cpp executable_retirement_test.cpp native_contract_test.cpp nontrivial_leaf_fail.cpp typed_operand_fail.cpp overloaded_operand_fail.cpp generic_conversion_fail.cpp nondeduced_generic_fail.cpp raw_interface_fail.cpp alignment_interface_fail.cpp program_metadata_fail.cpp
    EXAMPLES getting_started.cpp
    DOCS_INPUTS README.md docs/build.md docs/get-started.md docs/libraries.md
        docs/execution.md docs/reference.dox
    DOCS_EXCLUDE_SYMBOLS cel::detail cel::detail::*
    DOCS_OPTIONS "USE_MDFILE_AS_MAINPAGE = ${CMAKE_CURRENT_SOURCE_DIR}/README.md"
)
