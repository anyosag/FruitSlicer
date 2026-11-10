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

# Dear ImGui and Inter are declared in Dependencies.cmake (the engine uses them too).
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
