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
  add_executable(fruit_gui_tests gui/test_document.cpp ${PROJECT_SOURCE_DIR}/src/gui/Document.cpp)
  target_include_directories(fruit_gui_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/gui)
  target_link_libraries(fruit_gui_tests PRIVATE fruit_core Catch2::Catch2WithMain)
  catch_discover_tests(fruit_gui_tests)
endif()
