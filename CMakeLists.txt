cmake_minimum_required(VERSION 3.24)
project(FruitSlicer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(FRUIT_BUILD_TESTS "Build unit tests" ON)

include(cmake/Dependencies.cmake)

# Determinism: no fused multiply-add contraction, so results match across
# compilers/flags (support point hashes are compared in tests).
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic -ffp-contract=off)
endif()

add_subdirectory(src)

if(FRUIT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
