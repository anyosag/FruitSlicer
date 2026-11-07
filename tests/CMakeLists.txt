add_executable(fruit_tests
  test_geometry.cpp
  test_mesh.cpp
  test_slicer.cpp
  test_islands.cpp
  test_calibration.cpp
  test_points.cpp
  test_tree.cpp
  test_orient.cpp
  test_fit.cpp
  test_safety.cpp
  test_raster.cpp
  test_goo.cpp
  test_jobs.cpp
  test_api.cpp
  test_net.cpp
  test_model.cpp
)
target_link_libraries(fruit_tests PRIVATE fruit_core fruit_net_cli fruit_net_mock miniz Catch2::Catch2WithMain)
include(${catch2_SOURCE_DIR}/extras/Catch.cmake)
catch_discover_tests(fruit_tests)

# The desktop app's logic that needs no window (FRUIT_BUILD_APP=ON only).
if(FRUIT_BUILD_APP)
  add_executable(fruit_gui_tests gui/test_document.cpp gui/test_orient_logic.cpp gui/test_supports_logic.cpp
    gui/test_layers_logic.cpp gui/test_print_logic.cpp gui/test_send_logic.cpp gui/test_calibration_logic.cpp gui/test_jobfile_logic.cpp
    gui/test_settings_logic.cpp gui/test_resins_logic.cpp gui/test_plates_logic.cpp gui/test_fit_logic.cpp gui/test_ab_logic.cpp gui/test_goo_tools_logic.cpp
    ${PROJECT_SOURCE_DIR}/src/gui/Document.cpp ${PROJECT_SOURCE_DIR}/src/gui/Orient.cpp
    ${PROJECT_SOURCE_DIR}/src/gui/Supports.cpp ${PROJECT_SOURCE_DIR}/src/gui/Layers.cpp
    ${PROJECT_SOURCE_DIR}/src/gui/Print.cpp ${PROJECT_SOURCE_DIR}/src/gui/Send.cpp
    ${PROJECT_SOURCE_DIR}/src/gui/Calibration.cpp ${PROJECT_SOURCE_DIR}/src/gui/JobFile.cpp
    ${PROJECT_SOURCE_DIR}/src/gui/Preferences.cpp ${PROJECT_SOURCE_DIR}/src/gui/Setup.cpp
    ${PROJECT_SOURCE_DIR}/src/gui/Resins.cpp ${PROJECT_SOURCE_DIR}/src/gui/Plates.cpp
    ${PROJECT_SOURCE_DIR}/src/gui/Fit.cpp ${PROJECT_SOURCE_DIR}/src/gui/ABTest.cpp
    ${PROJECT_SOURCE_DIR}/src/gui/GooTools.cpp)
  target_include_directories(fruit_gui_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/gui)
  target_link_libraries(fruit_gui_tests PRIVATE fruit_core fruit_net fruit_net_mock fruit_licences miniz Catch2::Catch2WithMain)
  catch_discover_tests(fruit_gui_tests)
endif()
