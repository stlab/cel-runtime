cmake_minimum_required(VERSION 3.24)

include(cmake/CPM.cmake)

# Supply the unreleased package version until the repository has release tags.
set(CPP_LIBRARY_VERSION "0.0.0" CACHE STRING "CEL runtime package version")

# Fetch cpp-library before project()
CPMAddPackage("gh:stlab/cpp-library#b0b096964a93561c7dd1586fa7b0efdf5f077aab")
include(${cpp-library_SOURCE_DIR}/cpp-library.cmake)

# Enable dependency tracking before project()
cpp_library_enable_dependency_tracking()

# Now declare project
project(runtime LANGUAGES CXX)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable CTest infrastructure (required for tests/examples to work)
include(CTest)

# Setup library
cpp_library_setup(
    DESCRIPTION "Owned values and checked canonical type layouts for CEL evaluation"
    NAMESPACE cel
    REQUIRES_CPP_VERSION 20
    HEADERS runtime/layout.hpp runtime/value_type.hpp runtime/value.hpp
    SOURCES storage.cpp value_type.cpp value.cpp
    TESTS storage_test.cpp value_test.cpp resource_test.cpp nontrivial_leaf_fail.cpp
)
if(TARGET storage_test)
    target_include_directories(storage_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()
if(TARGET value_test)
    target_include_directories(value_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()
if(TARGET resource_test)
    target_include_directories(resource_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    set_tests_properties(resource_test PROPERTIES TIMEOUT 30)
endif()
