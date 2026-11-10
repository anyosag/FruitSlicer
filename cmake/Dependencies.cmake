include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# Clipper2 (Boost Software License 1.0) - 2D polygon booleans and offsets.
set(CLIPPER2_UTILS OFF CACHE BOOL "" FORCE)
set(CLIPPER2_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLIPPER2_TESTS OFF CACHE BOOL "" FORCE)
set(CLIPPER2_USINGZ OFF CACHE BOOL "" FORCE)
FetchContent_Declare(clipper2
  GIT_REPOSITORY https://github.com/AngusJohnson/Clipper2.git
  GIT_TAG Clipper2_1.4.0
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR CPP)

# CLI11 (BSD-3-Clause) - command line parsing.
FetchContent_Declare(cli11
  GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
  GIT_TAG v2.4.2
  GIT_SHALLOW TRUE)

# nlohmann/json (MIT) - reports, point files, coupon manifests.
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)

# cpp-httplib (MIT) - WebSocket and HTTP multipart client for Wi-Fi sending
# (src/net). Plain HTTP only: no TLS or compression libraries are linked.
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.62.0
  GIT_SHALLOW TRUE)

# miniz (MIT) - zip archives (3MF packages) and zlib (PNG).
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(INSTALL_PROJECT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(miniz
  GIT_REPOSITORY https://github.com/richgel999/miniz.git
  GIT_TAG 3.1.2
  GIT_SHALLOW TRUE)

# pugixml (MIT) - XML parsing (3MF model parts).
set(PUGIXML_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(pugixml
  GIT_REPOSITORY https://github.com/zeux/pugixml.git
  GIT_TAG v1.16
  GIT_SHALLOW TRUE)

# Manifold (Apache-2.0) - boolean operations, plane cuts and extrusion on
# closed meshes (src/ops). Serial build (no TBB, so nothing for the thread
# sanitizer to trip over); no 2D CrossSection, which wants a newer Clipper2
# than ours: letters are flattened and combined with our own Clipper2 and
# extruded with Manifold::Extrude, which needs no CrossSection.
set(MANIFOLD_TEST OFF CACHE BOOL "" FORCE)
set(MANIFOLD_PAR OFF CACHE BOOL "" FORCE)
set(MANIFOLD_CROSS_SECTION OFF CACHE BOOL "" FORCE)
set(MANIFOLD_CBIND OFF CACHE BOOL "" FORCE)
set(MANIFOLD_PYBIND OFF CACHE BOOL "" FORCE)
set(MANIFOLD_JSBIND OFF CACHE BOOL "" FORCE)
set(MANIFOLD_DOWNLOADS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(manifold
  GIT_REPOSITORY https://github.com/elalish/manifold.git
  GIT_TAG v3.5.4
  GIT_SHALLOW TRUE)

# Inter 4.1 (SIL Open Font License 1.1) - the UI typeface, also the face of
# the Label tool's letters. Dear ImGui (MIT) - imstb_truetype.h (stb_truetype)
# reads the letter outlines. The app uses both as well (AppDependencies.cmake).
FetchContent_Declare(inter
  URL https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip
  URL_HASH SHA256=9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.93.0
  GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(clipper2 cli11 json httplib miniz pugixml manifold inter imgui)

# Our code uses miniz's mz_ names only; its zlib-style aliases are unused
# static functions that warn in every file including miniz.h.
target_compile_definitions(miniz INTERFACE MINIZ_NO_ZLIB_COMPATIBLE_NAMES)

# Third-party code: their warnings are not ours to fix.
foreach(t miniz pugixml-static pugixml manifold)
  if(TARGET ${t})
    get_target_property(real ${t} ALIASED_TARGET)
    get_target_property(type ${t} TYPE)
    if(NOT real AND NOT type STREQUAL "INTERFACE_LIBRARY")
      target_compile_options(${t} PRIVATE -w)
    endif()
  endif()
endforeach()

if(FRUIT_BUILD_TESTS)
  # Catch2 (BSL-1.0) - unit tests.
  FetchContent_Declare(catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(catch2)
endif()
