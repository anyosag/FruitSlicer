add_library(fruit_core
  core/JobControl.cpp
  core/Parallel.cpp
  core/OutFile.cpp
  geometry/Transform.cpp
  geometry/Polygon.cpp
  mesh/TriangleMesh.cpp
  mesh/StlIO.cpp
  mesh/ObjIO.cpp
  mesh/Primitives.cpp
  mesh/AabbTree.cpp
  mesh/MeshAnalysis.cpp
  mesh/Placement.cpp
  mesh/ModelLoad.cpp
  slice/MeshSlicer.cpp
  layers/LayerStack.cpp
  layers/IslandGraph.cpp
  layers/HollowAnalysis.cpp
  printer/PrinterProfile.cpp
  io/PngWriter.cpp
  io/Render.cpp
  raster/Rasterizer.cpp
  formats/GooFile.cpp
  formats/GooExport.cpp
  formats/GooCompare.cpp
  formats/GooSetup.cpp
  support/SupportGeometry.cpp
  support/SupportModels.cpp
  support/CosmeticField.cpp
  support/PointGenerator.cpp
  support/SupportIO.cpp
  support/SupportTree.cpp
  support/SupportValidator.cpp
  support/OrientationSearch.cpp
  support/SupportSettings.cpp
  calibration/Coupons.cpp
  calibration/CalibrationPlates.cpp
  calibration/CalibrationFit.cpp
  calibration/ABPlate.cpp
)
target_include_directories(fruit_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fruit_core PUBLIC Clipper2 nlohmann_json::nlohmann_json)
find_package(Threads REQUIRED)
target_link_libraries(fruit_core PUBLIC Threads::Threads)

add_executable(fruitslicer app/main.cpp)
target_link_libraries(fruitslicer PRIVATE fruit_core CLI11::CLI11)
add_subdirectory(net)
