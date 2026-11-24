cmake_minimum_required(VERSION 3.24)
# Oldest macOS the builds run on; matches LSMinimumSystemVersion in the
# app's Info.plist. Ignored elsewhere.
set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0 CACHE STRING "Oldest macOS version to support")
project(FruitSlicer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(FRUIT_BUILD_TESTS "Build unit tests" ON)
option(FRUIT_BUILD_APP "Build the desktop app (SDL3 + OpenGL + Dear ImGui)" OFF)
# macOS: every copy of the app that runs is registered with Finder, and Finder
# opens a file with any copy of the app that owns it. Only the copy you use
# should be FruitSlicer itself (and open .fruitjob files): turn this on for it.
# Copies built without it, for tests, CI or other branches, are FruitSlicer's
# ".dev" and leave job files alone.
option(FRUIT_OPENS_JOB_FILES "macOS: this copy is FruitSlicer itself, the one Finder opens .fruitjob files with" OFF)
# Sending print files to the printer over Wi-Fi (src/net, cpp-httplib). Its
# network code is written for macOS and Linux; Windows copies save to a USB
# drive or a folder instead.
if(WIN32)
  set(fruit_wifi_default OFF)
else()
  set(fruit_wifi_default ON)
endif()
option(FRUIT_WIFI "Wi-Fi sending to the printer (src/net)" ${fruit_wifi_default})

include(cmake/Dependencies.cmake)

# Determinism: no fused multiply-add contraction, so results match across
# compilers/flags (support point hashes are compared in tests).
if(MSVC)
  # Microsoft's compiler (and clang-cl): sources are UTF-8 ("mm²", "⌘"),
  # standard C++, room for the big generated files. /fp:precise, the default,
  # never contracts a multiply and an add (Visual Studio 2022 and later).
  add_compile_options(/utf-8 /bigobj $<$<COMPILE_LANGUAGE:CXX>:/permissive-> $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic -ffp-contract=off)
endif()

# Windows programs: the main thread's stack as on macOS and Linux (8 MB, not
# Windows' 1 MB; the tests and the command line run the engine on it, and
# threads started with no size of their own get it too), and UTF-8 as their
# code page (cmake/fruit-utf8.manifest). Nothing elsewhere.
function(fruit_windows_exe target)
  if(MSVC)
    target_link_options(${target} PRIVATE /STACK:8388608)
    target_sources(${target} PRIVATE ${PROJECT_SOURCE_DIR}/cmake/fruit-utf8.manifest)
  endif()
endfunction()

# Every target sees the same FRUIT_WIFI (0 or 1): App's members depend on it.
add_compile_definitions(FRUIT_WIFI=$<BOOL:${FRUIT_WIFI}>)

add_subdirectory(src)

if(FRUIT_BUILD_APP)
  include(cmake/AppDependencies.cmake)
  add_subdirectory(src/gui)
endif()

if(FRUIT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
