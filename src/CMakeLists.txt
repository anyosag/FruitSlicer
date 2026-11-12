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
  mesh/FlatFaces.cpp
  mesh/MeshRepair.cpp
  mesh/Placement.cpp
  mesh/ModelLoad.cpp
  mesh/ModelCheck.cpp
  slice/MeshSlicer.cpp
  layers/LayerStack.cpp
  layers/AdaptiveLayers.cpp
  layers/IslandGraph.cpp
  layers/HollowAnalysis.cpp
  layers/Hollow.cpp
  layers/Drain.cpp
  printer/PrinterProfile.cpp
  io/PngWriter.cpp
  io/Render.cpp
  raster/Rasterizer.cpp
  formats/GooFile.cpp
  formats/GooExport.cpp
  formats/GooCompare.cpp
  formats/GooSetup.cpp
  formats/ThreeMf.cpp
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
  ops/MeshOps.cpp
  ops/Arrange.cpp
  ops/TextSolid.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/font_text_inter.cpp
)

# The Label tool's letters come from Inter Regular, built in like the app's
# own copies (src/gui) so engine builds without the app have it too.
set(text_font ${inter_SOURCE_DIR}/extras/ttf/Inter-Regular.ttf)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/font_text_inter.cpp
  COMMAND ${CMAKE_COMMAND} -DIN=${text_font} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/font_text_inter.cpp -DNAME=kFruitTextFontInter -P ${PROJECT_SOURCE_DIR}/cmake/Bin2Cpp.cmake
  DEPENDS ${text_font} ${PROJECT_SOURCE_DIR}/cmake/Bin2Cpp.cmake
  COMMENT "Embedding Inter Regular for text solids")
# stb_truetype (imstb_truetype.h) is read from Dear ImGui's folder, by this file only.
set_source_files_properties(ops/TextSolid.cpp PROPERTIES INCLUDE_DIRECTORIES ${imgui_SOURCE_DIR})
target_include_directories(fruit_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fruit_core PUBLIC Clipper2 nlohmann_json::nlohmann_json PRIVATE miniz pugixml::pugixml manifold)
find_package(Threads REQUIRED)
target_link_libraries(fruit_core PUBLIC Threads::Threads)

add_executable(fruitslicer app/main.cpp)
target_link_libraries(fruitslicer PRIVATE fruit_core CLI11::CLI11)
add_subdirectory(net)
