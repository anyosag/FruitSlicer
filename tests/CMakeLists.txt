add_executable(fruit_tests
  test_geometry.cpp
  test_mesh.cpp
  test_slicer.cpp
  test_islands.cpp
  test_calibration.cpp
  test_points.cpp
  test_tree.cpp
)
target_link_libraries(fruit_tests PRIVATE fruit_core Catch2::Catch2WithMain)
include(${catch2_SOURCE_DIR}/extras/Catch.cmake)
catch_discover_tests(fruit_tests)
