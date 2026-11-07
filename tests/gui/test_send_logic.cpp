// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Sending over Wi-Fi from the app: finding printers, sending, starting,
// cancelling, against the in-process mock printer on loopback.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <fstream>
#include <string>

#include "Send.h"
#include "net/testing/MockPrinter.h"

using Catch::Matchers::ContainsSubstring;
using fruit::net::testing::MockPrinter;
using fruit::net::testing::MockPrinterConfig;
using namespace gui;

namespace {

NetPorts portsFor(const MockPrinter& m) {
    NetPorts p;
    p.udp = m.udpPort();
    p.control = m.controlPort();
    p.broadcast = false;
    return p;
}

std::string goo(const std::string& name, std::size_t size) {
    const auto dir = std::filesystem::temp_directory_path() / "fruit_send_tests";
    std::filesystem::create_directories(dir);
    const auto p = dir / name;
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    for (std::size_t i = 0; i < size; ++i) out.put(static_cast<char>(i * 7 + 3));
    return p.string();
}

}  // namespace

TEST_CASE("Send: finds the printer at a known address, by name", "[gui][send]") {
    MockPrinterConfig cfg;
    cfg.name = "Workshop 16K";
    MockPrinter mock(cfg);
    const auto found = findPrinters({"127.0.0.1"}, portsFor(mock), fruit::JobControl::none());
    REQUIRE(found.size() == 1);
    CHECK(printerName(found[0]) == "Workshop 16K");
    CHECK(found[0].ip == "127.0.0.1");
    CHECK(probePrinter("127.0.0.1", portsFor(mock)).mainboardId == cfg.mainboardId);
    // A printer without a name goes by its model.
    fruit::net::PrinterInfo unnamed;
    unnamed.machineName = "Saturn 4 Ultra 16K";
    CHECK(printerName(unnamed) == "Saturn 4 Ultra 16K");
}

TEST_CASE("Send: the file arrives; the print starts only when asked", "[gui][send]") {
    MockPrinter mock;
    const std::string path = goo("bracket.goo", 300 * 1024);
    const auto printer = probePrinter("127.0.0.1", portsFor(mock));
    double last = -1;
    bool backwards = false;
    fruit::JobControl ctl([&](std::string_view, double f) {
        backwards = backwards || f < last;
        last = f;
    });
    const SendResult r = sendToPrinter(printer, path, false, portsFor(mock), ctl);
    CHECK_FALSE(r.started);
    CHECK(r.upload.size == 300 * 1024);
    CHECK(r.upload.remotePath == "/local/bracket.goo");
    CHECK(mock.files().count("/local/bracket.goo") == 1);
    CHECK(mock.startRequests() == 0);
    CHECK(last == 1.0);
    CHECK_FALSE(backwards);

    const SendResult s = sendToPrinter(printer, path, true, portsFor(mock), fruit::JobControl::none());
    CHECK(s.started);
    CHECK(mock.startRequests() >= 1);
    CHECK(mock.printing() == std::optional<std::string>("/local/bracket.goo"));
}

TEST_CASE("Send: cancelling leaves nothing on the printer; silence reads plainly", "[gui][send]") {
    MockPrinter mock;
    const auto printer = probePrinter("127.0.0.1", portsFor(mock));
    fruit::JobControl ctl;
    ctl.cancel();
    CHECK_THROWS_AS(sendToPrinter(printer, goo("cancelled.goo", 64 * 1024), true, portsFor(mock), ctl),
                    fruit::Cancelled);
    CHECK(mock.files().empty());
    CHECK(mock.startRequests() == 0);

    NetPorts nobody = portsFor(mock);
    nobody.udp = 9;  // the discard port: nothing answers
    CHECK_THROWS_WITH(probePrinter("127.0.0.1", nobody), ContainsSubstring("Nothing answered at 127.0.0.1"));
    CHECK(findPrinters({"127.0.0.1"}, nobody, fruit::JobControl::none()).empty());
}

TEST_CASE("Send: printer addresses added by hand are kept", "[gui][send]") {
    const auto dir = std::filesystem::temp_directory_path() / "fruit_send_tests" / "data";
    std::filesystem::remove_all(dir);
    const std::string file = (dir / "printers.json").string();
    CHECK(loadPrinterAddresses(file).empty());
    savePrinterAddresses(file, {"192.168.1.40", "printer.local"});
    CHECK(loadPrinterAddresses(file) == std::vector<std::string>{"192.168.1.40", "printer.local"});
    std::ofstream(file) << "not json";
    CHECK(loadPrinterAddresses(file).empty());
}
