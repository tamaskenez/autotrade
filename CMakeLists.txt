cmake_minimum_required(VERSION 3.31)
project(autotrade VERSION 0.1.0)

# Conan 2.x CMakeDeps creates per-config targets (CONAN_LIB::*_RELEASE/_DEBUG) with only
# IMPORTED_LOCATION_<CONFIG> set. CMake 4.x validates all imported targets at configure
# time, erroring on missing locations for other configs. Setting CMP0111 OLD turns this
# back into a warning — Conan's config files select the correct variant at link time.
cmake_policy(SET CMP0111 OLD)

set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF)

if(NOT CMAKE_PREFIX_PATH)
    set(CMAKE_PREFIX_PATH "${PROJECT_SOURCE_DIR}/_d")
endif()
message(STATUS "CMAKE_INSTALL_PREFIX: ${CMAKE_INSTALL_PREFIX}")

enable_testing()
set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set_property(GLOBAL PROPERTY USE_FOLDERS YES)

find_package(GTest REQUIRED CONFIG)
find_package(magic_enum REQUIRED CONFIG)
find_package(absl REQUIRED CONFIG)
find_package(CURL REQUIRED CONFIG)
find_package(reflectcpp REQUIRED CONFIG)


include(compiler_warnings.cmake)

set(CMAKE_COMPILE_WARNING_AS_ERROR 1)

if(MSVC)
    # PDB 'filename' was not found with 'object/library' or at 'path'; linking object as if no debug info
    add_link_options(/ignore:4099)
endif()

add_subdirectory(_submodules/meadow)
add_subdirectory(src)
