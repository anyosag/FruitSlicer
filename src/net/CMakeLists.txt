# Wi-Fi sending over SDCP V3 (see docs/wifi-sdcp.md). Independent of fruit_core.
add_library(fruit_net
  Md5.cpp
  Sdcp.cpp
  Discovery.cpp
  SdcpClient.cpp
)
target_include_directories(fruit_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(fruit_net PUBLIC nlohmann_json::nlohmann_json PRIVATE httplib::httplib)
# No "Expect: 100-continue" on uploads: the printer's HTTP server is not known
# to answer it (saturn4-bridge's fetch() never sends it). Set on every target
# that includes httplib.h so its inline functions stay identical.
set(FRUIT_HTTPLIB_DEFS CPPHTTPLIB_EXPECT_100_THRESHOLD=0)
target_compile_definitions(fruit_net PRIVATE ${FRUIT_HTTPLIB_DEFS})
find_package(Threads REQUIRED)
target_link_libraries(fruit_net PUBLIC Threads::Threads)

# The command line, as a library so tests can run it against the mock printer.
add_library(fruit_net_cli cli/PrinterCli.cpp)
target_link_libraries(fruit_net_cli PUBLIC fruit_net PRIVATE CLI11::CLI11)

add_executable(fruitslicer-printer cli/main.cpp)
target_link_libraries(fruitslicer-printer PRIVATE fruit_net_cli)

# In-process mock printer for the tests and the app's scripted runs (loopback only).
if(FRUIT_BUILD_TESTS OR FRUIT_BUILD_APP)
  add_library(fruit_net_mock testing/MockPrinter.cpp)
  target_link_libraries(fruit_net_mock PUBLIC fruit_net PRIVATE httplib::httplib)
  target_compile_definitions(fruit_net_mock PRIVATE ${FRUIT_HTTPLIB_DEFS})
endif()
