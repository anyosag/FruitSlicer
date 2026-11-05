add_library(fruit_core
  geometry/Transform.cpp
  geometry/Polygon.cpp
  mesh/TriangleMesh.cpp
  mesh/StlIO.cpp
  mesh/Primitives.cpp
  mesh/AabbTree.cpp
  mesh/MeshAnalysis.cpp
  mesh/Placement.cpp
  slice/MeshSlicer.cpp
  layers/LayerStack.cpp
  layers/IslandGraph.cpp
  printer/PrinterProfile.cpp
  io/PngWriter.cpp
  support/SupportGeometry.cpp
  calibration/Coupons.cpp
)
target_include_directories(fruit_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fruit_core PUBLIC Clipper2 nlohmann_json::nlohmann_json nanoflann::nanoflann)
find_package(Threads REQUIRED)
target_link_libraries(fruit_core PUBLIC Threads::Threads)

add_executable(fruitslicer app/main.cpp)
target_link_libraries(fruitslicer PRIVATE fruit_core CLI11::CLI11)
