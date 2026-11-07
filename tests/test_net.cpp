// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Wi-Fi sending (src/net) against the in-process mock printer, loopback only.
#include "net/Discovery.h"
#include "net/Md5.h"
#include "net/Sdcp.h"
#include "net/SdcpClient.h"
#include "net/cli/PrinterCli.h"
#include "net/testing/MockPrinter.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

using namespace fruit::net;
using fruit::net::testing::MockPrinter;
using fruit::net::testing::MockPrinterConfig;
using Catch::Matchers::ContainsSubstring;
using nlohmann::json;
using namespace std::chrono_literals;

namespace {

ClientOptions optionsFor(const MockPrinter& m) {
    ClientOptions o;
    o.udpPort = m.udpPort();
    o.controlPort = m.controlPort();
    o.probeTimeout = 1000ms;
    o.commandTimeout = 2000ms;
    return o;
}

StartOptions fastStart() {
    StartOptions s;
    s.pollInterval = 20ms;
    s.busyRetryDelay = 50ms;
    s.settleTimeout = 5s;
    return s;
}

std::filesystem::path tempFile(const std::string& name, std::size_t size, unsigned seed = 1) {
    auto dir = std::filesystem::temp_directory_path() / "fruit_net_tests";
    std::filesystem::create_directories(dir);
    auto p = dir / name;
    std::mt19937 rng(seed);
    std::string data(size, '\0');
    for (auto& c : data) c = char(rng());
    std::ofstream(p, std::ios::binary).write(data.data(), std::streamsize(data.size()));
    return p;
}

std::string readAll(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

int runCli(std::vector<std::string> args, std::string& out, std::string& err,
           const std::string& input = {}) {
    std::vector<const char*> argv{"fruitslicer-printer"};
    for (auto& a : args) argv.push_back(a.c_str());
    std::ostringstream o, e;
    std::istringstream in(input);
    const int rc = runPrinterCli(int(argv.size()), argv.data(), o, e, in);
    out = o.str();
    err = e.str();
    return rc;
}

}  // namespace

TEST_CASE("MD5 matches the RFC 1321 test suite", "[net]") {
    CHECK(Md5::hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(Md5::hex("a") == "0cc175b9c0f1b6a831c399e269772661");
    CHECK(Md5::hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(Md5::hex("message digest") == "f96b697d7cb7938d525a2f31aaf161d0");
    CHECK(Md5::hex("abcdefghijklmnopqrstuvwxyz") == "c3fcd3d76192e4007dfb496cca67e13b");
    CHECK(Md5::hex("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") ==
          "d174ab98d277d9f5a5611c2c9f419d9f");
    CHECK(Md5::hex("123456789012345678901234567890123456789012345678901234567890123456789012345678"
                   "90") == "57edf4a22be3c955ac49da2e2107b67a");
    // Streaming in odd pieces gives the same digest.
    std::string big(100000, 'x');
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = char(i * 31);
    Md5 m;
    for (std::size_t i = 0; i < big.size();) {
        const std::size_t n = std::min<std::size_t>(1 + i % 97, big.size() - i);
        m.update(big.data() + i, n);
        i += n;
    }
    CHECK(m.hexDigest() == Md5::hex(big));
}

TEST_CASE("Discovery replies parse in flat and nested form", "[net]") {
    const std::string flat = R"({"Id":"abc","Data":{"Name":"P","MachineName":"Saturn 4 Ultra 16K",
        "BrandName":"ELEGOO","MainboardIP":"192.168.1.20","MainboardID":"00001",
        "ProtocolVersion":"V3.0.0","FirmwareVersion":"V1.5.6"}})";
    auto p = parseDiscoveryReply(flat, "10.0.0.9");
    REQUIRE(p);
    CHECK(p->id == "abc");
    CHECK(p->mainboardId == "00001");
    CHECK(p->ip == "192.168.1.20");
    CHECK(p->machineName == "Saturn 4 Ultra 16K");

    const std::string nested = R"({"Id":"abc","Data":{"Attributes":{"Name":"P",
        "MainboardID":"00002","ProtocolVersion":"V3.0.0"},"Status":{"CurrentStatus":[0]}}})";
    p = parseDiscoveryReply(nested, "10.0.0.9");
    REQUIRE(p);
    CHECK(p->mainboardId == "00002");
    CHECK(p->ip == "10.0.0.9");  // no MainboardIP: fall back to the sender

    CHECK_FALSE(parseDiscoveryReply("M99999", "1.2.3.4"));
    CHECK_FALSE(parseDiscoveryReply(R"({"Id":"x","Data":{}})", "1.2.3.4"));
}

TEST_CASE("Request envelope follows the spec", "[net]") {
    PrinterInfo p;
    p.id = "ID";
    p.mainboardId = "MB";
    const json r = makeRequest(p, 128, json{{"Filename", "/local/a.goo"}, {"StartLayer", 0}}, "0011", 1687069655);
    CHECK(r["Id"] == "ID");
    CHECK(r["Topic"] == "sdcp/request/MB");
    CHECK(r["Data"]["Cmd"] == 128);
    CHECK(r["Data"]["RequestID"] == "0011");
    CHECK(r["Data"]["MainboardID"] == "MB");
    CHECK(r["Data"]["TimeStamp"] == 1687069655);
    CHECK(r["Data"]["From"] == 0);
    CHECK(r["Data"]["Data"]["Filename"] == "/local/a.goo");
}

TEST_CASE("Status accepts CurrentStatus as array or number", "[net]") {
    auto s = parseStatus(json::parse(R"({"CurrentStatus":[1,2],"PrintInfo":{"Status":3,
        "CurrentLayer":5,"TotalLayer":10,"Filename":"a.goo"}})"));
    CHECK(s.has(MachineStatus::Printing));
    CHECK(s.has(MachineStatus::FileTransferring));
    REQUIRE(s.print);
    CHECK(s.print->currentLayer == 5);
    s = parseStatus(json::parse(R"({"CurrentStatus":0})"));
    CHECK(s.idle());
}

TEST_CASE("Discovery finds the mock printer over loopback", "[net]") {
    for (bool nested : {false, true}) {
        MockPrinterConfig cfg;
        cfg.nestedDiscovery = nested;
        MockPrinter mock(cfg);
        DiscoveryOptions o;
        o.broadcast = false;
        o.unicastTargets = {"127.0.0.1"};
        o.port = mock.udpPort();
        o.timeout = 300ms;
        const auto found = discover(o);
        REQUIRE(found.size() == 1);
        CHECK(found[0].mainboardId == cfg.mainboardId);
        CHECK(found[0].ip == "127.0.0.1");
        CHECK(probe("127.0.0.1", mock.udpPort(), 1000ms).id == cfg.id);
    }
}

TEST_CASE("Probe of a silent address times out", "[net]") {
    // A bound UDP socket that never answers.
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
    socklen_t len = sizeof a;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    CHECK_THROWS_AS(probe("127.0.0.1", ntohs(a.sin_port), 200ms), TimeoutError);
    ::close(fd);
}

TEST_CASE("Status, attributes and file list", "[net]") {
    MockPrinter mock;
    mock.addFile("/local/old.goo", "x");
    SdcpClient c("127.0.0.1", optionsFor(mock));
    CHECK(c.printer().machineName == "Saturn 4 Ultra 16K");
    const auto st = c.status();
    CHECK(st.idle());
    REQUIRE(st.uvLedTemp);
    const auto attrs = c.attributes();
    CHECK(attrs["Resolution"] == "15120x6230");
    const auto files = c.files();
    REQUIRE(files.size() == 1);
    CHECK(files[0].name == "/local/old.goo");
    CHECK_FALSE(files[0].isFolder);
    CHECK(mock.rejectedRequests() == 0);
}

TEST_CASE("Silent commands time out instead of hanging", "[net]") {
    MockPrinter mock;
    SdcpClient c("127.0.0.1", optionsFor(mock));
    CHECK_THROWS_AS(c.command(Cmd(132), json::object(), 200ms), TimeoutError);
    // The connection is still usable afterwards.
    CHECK(c.status().idle());
}

TEST_CASE("Chunked upload stores the exact file and reports progress", "[net]") {
    MockPrinter mock;
    auto opts = optionsFor(mock);
    opts.chunkSize = 256 * 1024;
    SdcpClient c("127.0.0.1", opts);
    const auto path = tempFile("part.goo", 700 * 1024 + 13);
    std::vector<std::uint64_t> sent;
    const auto r = c.upload(path, [&](const UploadProgress& p) {
        sent.push_back(p.sent);
        CHECK(p.total == std::filesystem::file_size(path));
    });
    CHECK(r.remotePath == "/local/part.goo");
    CHECK(r.md5 == Md5::hexOfFile(path));
    CHECK(mock.chunksReceived() == 3);
    REQUIRE(sent.size() == 3);
    CHECK(sent.back() == std::filesystem::file_size(path));
    CHECK(std::is_sorted(sent.begin(), sent.end()));
    const auto files = mock.files();
    REQUIRE(files.count("/local/part.goo"));
    CHECK(files.at("/local/part.goo") == readAll(path));
}

TEST_CASE("A corrupted transfer fails the MD5 check", "[net]") {
    MockPrinterConfig cfg;
    cfg.corruptUploads = true;
    MockPrinter mock(cfg);
    SdcpClient c("127.0.0.1", optionsFor(mock));
    const auto path = tempFile("bad.goo", 50000);
    CHECK_THROWS_AS(c.upload(path), UploadError);
    CHECK(mock.files().empty());
}

TEST_CASE("Cancelling an upload stops it and sends Cmd 255", "[net]") {
    MockPrinter mock;
    auto opts = optionsFor(mock);
    opts.chunkSize = 64 * 1024;
    SdcpClient c("127.0.0.1", opts);
    const auto path = tempFile("cancel.goo", 512 * 1024);
    std::atomic<bool> cancel{false};
    CHECK_THROWS_AS(c.upload(path, [&](const UploadProgress& p) {
        if (p.sent >= 128 * 1024) cancel = true;
    }, &cancel), CancelledError);
    CHECK(mock.cancelRequests() == 1);
    CHECK(mock.chunksReceived() == 2);
    CHECK(mock.files().empty());
    CHECK(c.status().idle());
}

TEST_CASE("Start after upload waits for verification", "[net]") {
    MockPrinterConfig cfg;
    cfg.verifyTime = 400ms;
    MockPrinter mock(cfg);
    SdcpClient c("127.0.0.1", optionsFor(mock));
    const auto r = c.upload(tempFile("go.goo", 10000));
    CHECK(c.status().has(MachineStatus::FileTransferring));
    c.startPrint(r.remotePath, fastStart());
    CHECK(mock.printing() == "/local/go.goo");
    CHECK(mock.busyAnswers() == 0);  // never asked while verifying
}

TEST_CASE("Start retries while the printer answers busy", "[net]") {
    MockPrinterConfig cfg;
    cfg.verifyTime = 50ms;
    cfg.extraBusyTime = 300ms;  // idle status but Cmd 128 still busy, as on the 16K
    MockPrinter mock(cfg);
    SdcpClient c("127.0.0.1", optionsFor(mock));
    const auto r = c.upload(tempFile("busy.goo", 10000));
    auto so = fastStart();
    so.busyRetries = 20;
    std::vector<std::string> log;
    so.log = [&](const std::string& s) { log.push_back(s); };
    c.startPrint(r.remotePath, so);
    CHECK(mock.printing() == "/local/busy.goo");
    CHECK(mock.busyAnswers() >= 1);
    CHECK_FALSE(log.empty());
}

TEST_CASE("Start gives up on a printer that stays busy", "[net]") {
    MockPrinterConfig cfg;
    cfg.forcedStartAck = 1;
    MockPrinter mock(cfg);
    mock.addFile("/local/a.goo", "x");
    SdcpClient c("127.0.0.1", optionsFor(mock));
    auto so = fastStart();
    so.busyRetries = 2;
    try {
        c.startPrint("/local/a.goo", so);
        FAIL("expected AckError");
    } catch (const AckError& e) {
        CHECK(e.ack() == 1);
        CHECK_THAT(e.what(), ContainsSubstring("busy"));
    }
    CHECK(mock.startRequests() == 3);
}

TEST_CASE("Every start-print Ack code has a clear error", "[net]") {
    const std::vector<std::pair<int, std::string>> cases = {
        {2, "not found"},  {3, "MD5"},   {4, "read"},
        {5, "resolution"}, {6, "format"}, {7, "model"}};
    for (const auto& [ack, words] : cases) {
        MockPrinterConfig cfg;
        cfg.forcedStartAck = ack;
        MockPrinter mock(cfg);
        SdcpClient c("127.0.0.1", optionsFor(mock));
        try {
            c.startPrint("/local/a.goo", fastStart());
            FAIL("expected AckError for " << ack);
        } catch (const AckError& e) {
            CHECK(e.ack() == ack);
            CHECK(e.cmd() == 128);
            CHECK_THAT(e.what(), ContainsSubstring(words));
        }
        CHECK(mock.startRequests() == 1);  // only busy is retried
    }
}

TEST_CASE("Start refuses while a print is running", "[net]") {
    MockPrinter mock;
    mock.addFile("/local/a.goo", "x");
    mock.setPrinting("/local/other.goo");
    SdcpClient c("127.0.0.1", optionsFor(mock));
    CHECK_THROWS_AS(c.startPrint("/local/a.goo", fastStart()), NetError);
    CHECK(mock.startRequests() == 0);
}

TEST_CASE("CLI: discover, status and files", "[net][cli]") {
    MockPrinter mock;
    mock.addFile("/local/x.goo", "x");
    const std::string udp = std::to_string(mock.udpPort());
    const std::string ctl = std::to_string(mock.controlPort());
    std::string out, err;
    REQUIRE(runCli({"--json", "--udp-port", udp, "discover", "--no-broadcast", "--ask", "127.0.0.1",
                    "--timeout", "300"}, out, err) == 0);
    const json found = json::parse(out);
    REQUIRE(found.size() == 1);
    CHECK(found[0]["machine"] == "Saturn 4 Ultra 16K");

    REQUIRE(runCli({"--udp-port", udp, "--port", ctl, "status", "127.0.0.1"}, out, err) == 0);
    CHECK_THAT(out, ContainsSubstring("state:    idle"));
    REQUIRE(runCli({"--udp-port", udp, "--port", ctl, "status", "127.0.0.1", "--json"}, out, err) == 0);
    CHECK(json::parse(out)["status"]["CurrentStatus"] == json::array({0}));

    REQUIRE(runCli({"--udp-port", udp, "--port", ctl, "files", "127.0.0.1"}, out, err) == 0);
    CHECK_THAT(out, ContainsSubstring("/local/x.goo"));
}

TEST_CASE("CLI: send --start asks before moving hardware", "[net][cli]") {
    MockPrinterConfig cfg;
    cfg.verifyTime = 50ms;
    MockPrinter mock(cfg);
    const std::string udp = std::to_string(mock.udpPort());
    const std::string ctl = std::to_string(mock.controlPort());
    const auto path = tempFile("cli.goo", 30000).string();
    std::string out, err;

    // Declined: nothing is uploaded or started.
    CHECK(runCli({"--udp-port", udp, "--port", ctl, "send", "127.0.0.1", path, "--start"}, out, err,
                 "no\n") == 4);
    CHECK_THAT(err, ContainsSubstring("Type 'yes'"));
    CHECK(mock.files().empty());
    CHECK(mock.startRequests() == 0);

    // No --start: upload only, no confirmation needed.
    CHECK(runCli({"--udp-port", udp, "--port", ctl, "send", "127.0.0.1", path}, out, err) == 0);
    CHECK(mock.files().count("/local/cli.goo") == 1);
    CHECK(mock.startRequests() == 0);

    // Confirmed: uploads and starts.
    CHECK(runCli({"--json", "--udp-port", udp, "--port", ctl, "send", "127.0.0.1", path, "--start"},
                 out, err, "yes\n") == 0);
    CHECK(json::parse(out)["started"] == true);
    CHECK(mock.printing() == "/local/cli.goo");

    // Already printing: --start is refused before uploading.
    CHECK(runCli({"--udp-port", udp, "--port", ctl, "send", "127.0.0.1", path, "--start", "--yes"},
                 out, err) == 3);
}
