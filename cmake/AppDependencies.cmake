# Dependencies of the desktop app (FRUIT_BUILD_APP=ON only).
include(FetchContent)
enable_language(C)
if(APPLE)
  enable_language(OBJCXX)
endif()

# SDL 3 (zlib) - window, OpenGL context, input, native file dialogs.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
foreach(sub AUDIO CAMERA HAPTIC HIDAPI JOYSTICK SENSOR GPU VULKAN RENDER_VULKAN RENDER_GPU)
  set(SDL_${sub} OFF CACHE BOOL "" FORCE)
endforeach()
if(UNIX AND NOT APPLE)
  set(SDL_WAYLAND OFF CACHE BOOL "" FORCE)  # X11 is enough for development and CI
endif()
FetchContent_Declare(sdl3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG release-3.4.20
  GIT_SHALLOW TRUE)

# Dear ImGui (MIT) - immediate-mode UI. No CMake project of its own.
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.93.0
  GIT_SHALLOW TRUE)

# Inter 4.1 (SIL Open Font License 1.1) - the UI typeface.
FetchContent_Declare(inter
  URL https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip
  URL_HASH SHA256=9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e)

FetchContent_MakeAvailable(sdl3 imgui inter)

add_library(fruit_imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(fruit_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(fruit_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS "IMGUI_USER_CONFIG=\"fruit_imconfig.h\"")
target_include_directories(fruit_imgui PUBLIC ${PROJECT_SOURCE_DIR}/src/gui/imconfig)
target_link_libraries(fruit_imgui PUBLIC SDL3::SDL3-static)

add_library(fruit_glad STATIC ${PROJECT_SOURCE_DIR}/third_party/glad/src/gl.c)
target_include_directories(fruit_glad PUBLIC ${PROJECT_SOURCE_DIR}/third_party/glad/include)

# Third-party code: their warnings are not ours to fix.
foreach(t SDL3-static fruit_imgui fruit_glad)
  if(TARGET ${t})
    target_compile_options(${t} PRIVATE -w)
  endif()
endforeach()
