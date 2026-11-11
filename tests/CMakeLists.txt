add_executable(fruit_tests
  test_geometry.cpp
  test_mesh.cpp
  test_slicer.cpp
  test_islands.cpp
  test_hollow.cpp
  test_drain.cpp
  test_hollow_print.cpp
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
  test_repair.cpp
  test_ops.cpp
)
target_link_libraries(fruit_tests PRIVATE fruit_core fruit_net_cli fruit_net_mock miniz Catch2::Catch2WithMain)
include(${catch2_SOURCE_DIR}/extras/Catch.cmake)
catch_discover_tests(fruit_tests)

# The desktop app's logic that needs no window (FRUIT_BUILD_APP=ON only).
if(FRUIT_BUILD_APP)
  add_executable(fruit_gui_tests gui/test_document.cpp gui/test_orient_logic.cpp gui/test_supports_logic.cpp
    gui/test_layers_logic.cpp gui/test_print_logic.cpp gui/test_send_logic.cpp gui/test_calibration_logic.cpp gui/test_jobfile_logic.cpp
    gui/test_settings_logic.cpp gui/test_resins_logic.cpp gui/test_plates_logic.cpp gui/test_fit_logic.cpp gui/test_ab_logic.cpp gui/test_goo_tools_logic.cpp gui/test_hollow_logic.cpp gui/test_repair_logic.cpp gui/test_placing_logic.cpp gui/test_workspace_logic.cpp gui/test_parts_logic.cpp gui/test_mirror_logic.cpp gui/test_measure_logic.cpp gui/test_parts_tools_logic.cpp gui/test_shape_tools_logic.cpp gui/test_tools_layout.cpp
    ${PROJECT_SOURCE_DIR}/src/gui/Document.cpp ${PROJECT_SOURCE_DIR}/src/gui/Parts.cpp ${PROJECT_SOURCE_DIR}/src/gui/PartsTools.cpp ${PROJECT_SOURCE_DIR}/src/gui/Collisions.cpp ${PROJECT_SOURCE_DIR}/src/gui/Orient.cpp
    ${PROJECT_SOURCE_DIR}/src/gui/Supports.cpp ${PROJECT_SOURCE_DIR}/src/gui/Layers.cpp
    ${PROJECT_SOURCE_DIR}/src/gui/Print.cpp ${PROJECT_SOURCE_DIR}/src/gui/Send.cpp
    ${PROJECT_SOURCE_DIR}/src/gui/Calibration.cpp ${PROJECT_SOURCE_DIR}/src/gui/JobFile.cpp
    ${PROJECT_SOURCE_DIR}/src/gui/Preferences.cpp ${PROJECT_SOURCE_DIR}/src/gui/Setup.cpp
    ${PROJECT_SOURCE_DIR}/src/gui/Resins.cpp ${PROJECT_SOURCE_DIR}/src/gui/Plates.cpp
    ${PROJECT_SOURCE_DIR}/src/gui/Fit.cpp ${PROJECT_SOURCE_DIR}/src/gui/ABTest.cpp
    ${PROJECT_SOURCE_DIR}/src/gui/GooTools.cpp ${PROJECT_SOURCE_DIR}/src/gui/Hollows.cpp
    ${PROJECT_SOURCE_DIR}/src/gui/Repairs.cpp ${PROJECT_SOURCE_DIR}/src/gui/Placing.cpp ${PROJECT_SOURCE_DIR}/src/gui/Measure.cpp ${PROJECT_SOURCE_DIR}/src/gui/ShapeTools.cpp
    ${PROJECT_SOURCE_DIR}/src/gui/Navigation.cpp ${PROJECT_SOURCE_DIR}/src/gui/ViewCube.cpp ${PROJECT_SOURCE_DIR}/src/gui/ToolsLayout.cpp
    ${PROJECT_SOURCE_DIR}/src/gui/view3d/Camera.cpp)
  target_include_directories(fruit_gui_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/gui)
  target_link_libraries(fruit_gui_tests PRIVATE fruit_core fruit_net fruit_net_mock fruit_licences miniz Catch2::Catch2WithMain)
  catch_discover_tests(fruit_gui_tests)
endif()
