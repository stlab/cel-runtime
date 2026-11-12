cmake_minimum_required(VERSION 3.24)

include(cmake/CPM.cmake)

set(CPP_LIBRARY_VERSION "" CACHE STRING "CEL runtime package version override")

# Fetch cpp-library before project()
CPMAddPackage("gh:stlab/cpp-library@5.8.1")
include(${cpp-library_SOURCE_DIR}/cpp-library.cmake)

# Enable dependency tracking before project()
cpp_library_enable_dependency_tracking()

# Now declare project
project(runtime LANGUAGES CXX)
set(CMAKE_CXX_EXTENSIONS OFF)

if(PROJECT_IS_TOP_LEVEL)
    include(CTest)
endif()

# Setup library
cpp_library_setup(
    DESCRIPTION "Checked CEL programs, owned values, and reusable execution stacks"
    NAMESPACE cel
    REQUIRES_CPP_VERSION 20
    HEADERS runtime.hpp runtime/layout.hpp runtime/value_type.hpp runtime/value.hpp runtime/raw_stack.hpp runtime/dyn_segment.hpp runtime/callable.hpp runtime/detail/retirement.hpp runtime/detail/frame.hpp runtime/detail/raw_segment.hpp
    SOURCES storage.cpp value_type.cpp value.cpp raw_stack.cpp raw_segment.cpp dyn_segment.cpp callable.cpp
    TESTS storage_test.cpp value_test.cpp resource_test.cpp raw_execution_test.cpp segment_test.cpp composite_execution_test.cpp callable_test.cpp executable_retirement_test.cpp native_contract_test.cpp nontrivial_leaf_fail.cpp typed_operand_fail.cpp overloaded_operand_fail.cpp generic_conversion_fail.cpp nondeduced_generic_fail.cpp raw_interface_fail.cpp alignment_interface_fail.cpp program_metadata_fail.cpp
    EXAMPLES getting_started.cpp
    DOCS_INPUTS README.md docs/build.md docs/get-started.md docs/libraries.md
        docs/execution.md docs/reference.dox
    DOCS_OPTIONS "USE_MDFILE_AS_MAINPAGE = ${CMAKE_CURRENT_SOURCE_DIR}/README.md"
        "EXCLUDE_SYMBOLS = cel::detail cel::detail::*"
        "FULL_PATH_NAMES = NO"
        "STRIP_FROM_PATH = ${CMAKE_CURRENT_SOURCE_DIR}"
        "STRIP_FROM_INC_PATH = ${CMAKE_CURRENT_SOURCE_DIR}/include"
)
if(TARGET docs)
    # Keeps checkout-relative contract-test links usable in generated documentation.
    add_custom_command(TARGET docs POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/tests" "${CMAKE_CURRENT_BINARY_DIR}/tests"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/examples" "${CMAKE_CURRENT_BINARY_DIR}/examples"
        COMMAND "${CMAKE_COMMAND}" -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt" "${CMAKE_CURRENT_BINARY_DIR}/CMakeLists.txt")
endif()
if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTING)
    configure_file(tests/installed_consumer.cmake.in installed_consumer.cmake @ONLY)
    configure_file(tests/source_consumer.cmake.in source_consumer.cmake @ONLY)
    add_test(NAME installed_consumer
        COMMAND "${CMAKE_COMMAND}" "-DCONFIG=$<CONFIG>"
            -P "${CMAKE_CURRENT_BINARY_DIR}/installed_consumer.cmake")
    add_test(NAME source_consumer
        COMMAND "${CMAKE_COMMAND}" "-DCONFIG=$<CONFIG>"
            -P "${CMAKE_CURRENT_BINARY_DIR}/source_consumer.cmake")
    set_tests_properties(installed_consumer source_consumer PROPERTIES TIMEOUT 180)
    set_tests_properties(executable_retirement_test PROPERTIES TIMEOUT 180)
endif()
if(TARGET storage_test)
    target_include_directories(storage_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()
if(TARGET raw_execution_test)
    target_include_directories(raw_execution_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    set_tests_properties(raw_execution_test PROPERTIES TIMEOUT 30)
endif()
if(TARGET value_test)
    target_include_directories(value_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()
if(TARGET resource_test)
    target_include_directories(resource_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    set_tests_properties(resource_test PROPERTIES TIMEOUT 30)
endif()
if(TARGET segment_test)
    target_include_directories(segment_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    set_tests_properties(segment_test PROPERTIES TIMEOUT 30)
endif()
foreach(test callable_test executable_retirement_test)
    if(TARGET ${test})
        target_include_directories(${test} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    endif()
endforeach()
if(TARGET composite_execution_test)
    target_include_directories(composite_execution_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    set_tests_properties(composite_execution_test PROPERTIES TIMEOUT 30)
endif()
if(TARGET callable_test)
    target_include_directories(callable_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    set_tests_properties(callable_test PROPERTIES TIMEOUT 30)
endif()
