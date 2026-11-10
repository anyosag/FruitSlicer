# The FruitSlicer desktop app: SDL3 window, OpenGL 3.3 core, Dear ImGui.

# Inter, embedded so the app needs no font files at run time.
set(FRUIT_FONTS Regular Medium SemiBold Bold)
set(font_sources)
foreach(w ${FRUIT_FONTS})
  set(src ${inter_SOURCE_DIR}/extras/ttf/Inter-${w}.ttf)
  set(out ${CMAKE_CURRENT_BINARY_DIR}/font_inter_${w}.cpp)
  string(TOLOWER ${w} lw)
  add_custom_command(OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -DIN=${src} -DOUT=${out} -DNAME=kFontInter_${lw} -P ${PROJECT_SOURCE_DIR}/cmake/Bin2Cpp.cmake
    DEPENDS ${src} ${PROJECT_SOURCE_DIR}/cmake/Bin2Cpp.cmake
    COMMENT "Embedding Inter ${w}")
  list(APPEND font_sources ${out})
endforeach()

# The licence texts of FruitSlicer and of everything built into the app,
# embedded from the files this build uses (shown under About).
set(FRUIT_LICENCE_FILES
  fruitslicer ${PROJECT_SOURCE_DIR}/LICENSE
  clipper2 ${clipper2_SOURCE_DIR}/LICENSE
  manifold ${manifold_SOURCE_DIR}/LICENSE
  json ${json_SOURCE_DIR}/LICENSE.MIT
  httplib ${httplib_SOURCE_DIR}/LICENSE
  miniz ${miniz_SOURCE_DIR}/LICENSE
  pugixml ${pugixml_SOURCE_DIR}/LICENSE.md
  sdl3 ${sdl3_SOURCE_DIR}/LICENSE.txt
  sdl3_yuv2rgb ${sdl3_SOURCE_DIR}/src/video/yuv2rgb/LICENSE
  imgui ${imgui_SOURCE_DIR}/LICENSE.txt
  glad ${PROJECT_SOURCE_DIR}/third_party/glad/LICENSES.txt
  inter ${inter_SOURCE_DIR}/LICENSE.txt)
set(licence_sources)
list(LENGTH FRUIT_LICENCE_FILES n)
math(EXPR last "${n} - 1")
foreach(i RANGE 0 ${last} 2)
  math(EXPR j "${i} + 1")
  list(GET FRUIT_LICENCE_FILES ${i} id)
  list(GET FRUIT_LICENCE_FILES ${j} src)
  if(NOT EXISTS ${src})
    message(FATAL_ERROR "licence file missing: ${src}")
  endif()
  set(out ${CMAKE_CURRENT_BINARY_DIR}/licence_${id}.cpp)
  add_custom_command(OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -DIN=${src} -DOUT=${out} -DNAME=kLicence_${id} -P ${PROJECT_SOURCE_DIR}/cmake/Bin2Cpp.cmake
    DEPENDS ${src} ${PROJECT_SOURCE_DIR}/cmake/Bin2Cpp.cmake
    COMMENT "Embedding the ${id} licence")
  list(APPEND licence_sources ${out})
endforeach()
add_library(fruit_licences STATIC Licences.cpp ${licence_sources})
target_include_directories(fruit_licences PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

set(app_sources
  main.cpp
  ABTest.cpp
  App.cpp
  Calibration.cpp
  Capture.cpp
  Dialogs.cpp
  Document.cpp
  Fit.cpp
  GooTools.cpp
  Hollows.cpp
  Repairs.cpp
  Icons.cpp
  JobFile.cpp
  Jobs.cpp
  Layers.cpp
  Log.cpp
  Navigation.cpp
  ViewCube.cpp
  Orient.cpp
  Placing.cpp
  Plates.cpp
  Preferences.cpp
  Print.cpp
  Resins.cpp
  Send.cpp
  Setup.cpp
  Script.cpp
  Supports.cpp
  Theme.cpp
  Widgets.cpp
  screens/ABScreens.cpp
  screens/AboutScreen.cpp
  screens/CalibrateScreens.cpp
  screens/HollowScreen.cpp
  screens/HomeScreen.cpp
  screens/JobDialogs.cpp
  screens/JobPanel.cpp
  screens/JobScreen.cpp
  screens/LayersStep.cpp
  screens/PlaceHandles.cpp
  screens/PlateScreens.cpp
  screens/PrintStep.cpp
  screens/ResinsScreen.cpp
  screens/SendDialog.cpp
  screens/SettingsScreens.cpp
  screens/SupportsAdvanced.cpp
  screens/OrientStep.cpp
  screens/SupportsStep.cpp
  screens/ToolsScreens.cpp
  screens/TipEdit.cpp
  screens/Workspace.cpp
  view3d/Camera.cpp
  view3d/Renderer.cpp
  platform/Platform.cpp
  ${font_sources})
if(APPLE)
  list(APPEND app_sources platform/PlatformMac.mm macos/FruitSlicer.icns)
  set_source_files_properties(platform/PlatformMac.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
endif()

add_executable(fruitslicer_app ${app_sources})
target_include_directories(fruitslicer_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(fruitslicer_app PRIVATE GL_SILENCE_DEPRECATION FRUIT_VERSION="${PROJECT_VERSION}")
# fruit_net_mock: an SDCP printer on loopback, started only by scripted runs
# ({"mock_printer": true}) so the Wi-Fi screens can be captured without a printer.
target_link_libraries(fruitslicer_app PRIVATE fruit_core fruit_net fruit_net_mock fruit_imgui fruit_glad fruit_licences miniz)

if(APPLE)
  set_source_files_properties(macos/FruitSlicer.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  set_target_properties(fruitslicer_app PROPERTIES
    OUTPUT_NAME FruitSlicer
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in
    MACOSX_BUNDLE_BUNDLE_NAME FruitSlicer
    MACOSX_BUNDLE_GUI_IDENTIFIER io.github.anyosag.fruitslicer
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_ICON_FILE FruitSlicer.icns)
  target_link_libraries(fruitslicer_app PRIVATE "-framework Foundation" "-framework AppKit")

  # The licence texts of everything compiled into the app travel with it,
  # as the MIT, Apache and OFL licences ask.
  set(lic_dir "$<TARGET_BUNDLE_CONTENT_DIR:fruitslicer_app>/Resources/Licenses")
  set(licences
    FruitSlicer ${PROJECT_SOURCE_DIR}/LICENSE
    Clipper2 ${clipper2_SOURCE_DIR}/LICENSE
    Manifold ${manifold_SOURCE_DIR}/LICENSE
    nlohmann-json ${json_SOURCE_DIR}/LICENSE.MIT
    cpp-httplib ${httplib_SOURCE_DIR}/LICENSE
    SDL3 ${sdl3_SOURCE_DIR}/LICENSE.txt
    DearImGui ${imgui_SOURCE_DIR}/LICENSE.txt
    glad ${PROJECT_SOURCE_DIR}/third_party/glad/LICENSES.txt
    miniz ${miniz_SOURCE_DIR}/LICENSE
    pugixml ${pugixml_SOURCE_DIR}/LICENSE.md
    Inter ${inter_SOURCE_DIR}/LICENSE.txt)
  set(copy_licences)
  while(licences)
    list(POP_FRONT licences name file)
    list(APPEND copy_licences COMMAND ${CMAKE_COMMAND} -E copy_if_different ${file} ${lic_dir}/${name}.txt)
  endwhile()
  # Signed ad hoc after every build, so the bundle (resources included) is
  # sealed. Without a Developer ID, macOS asks once before the first launch.
  add_custom_command(TARGET fruitslicer_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${lic_dir}
    ${copy_licences}
    COMMAND codesign --force --sign - --timestamp=none $<TARGET_BUNDLE_DIR:fruitslicer_app>
    COMMENT "Adding licence texts and signing FruitSlicer.app"
    VERBATIM)
endif()
