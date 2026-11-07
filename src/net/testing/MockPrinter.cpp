// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "net/testing/MockPrinter.h"

#include "net/Md5.h"
#include "net/Sdcp.h"

#include <httplib.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace fruit::net::testing {

using nlohmann::json;
using Clock = std::chrono::steady_clock;

struct MockPrinter::Http {
    httplib::Server server;
};

namespace {

std::int64_t unixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

json uploadFailure(int code) {
    return json{{"code", "111111"},
                {"messages", json::array({{{"field", "common_field"}, {"message", code}}})},
                {"data", nullptr},
                {"success", false}};
}

json uploadSuccess() {
    return json{{"code", "000000"}, {"messages", nullptr}, {"data", json::object()}, {"success", true}};
}

}  // namespace

MockPrinter::MockPrinter(MockPrinterConfig config)
    : cfg_(std::move(config)), http_(std::make_unique<Http>()) {
    // ---- UDP discovery responder ----
    udpFd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpFd_ < 0) throw std::runtime_error("mock: socket failed");
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (::bind(udpFd_, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0)
        throw std::runtime_error("mock: udp bind failed");
    socklen_t len = sizeof a;
    ::getsockname(udpFd_, reinterpret_cast<sockaddr*>(&a), &len);
    udpPort_ = ntohs(a.sin_port);

    // ---- HTTP upload ----
    auto& svr = http_->server;
    svr.Post(std::string(kUploadPath), [this](const httplib::Request& req, httplib::Response& res) {
        auto reply = [&](const json& j) { res.set_content(j.dump(), "application/json"); };
        const auto& f = req.form;
        for (const char* k : {"S-File-MD5", "Check", "Offset", "Uuid", "TotalSize"})
            if (!f.has_field(k)) return reply(uploadFailure(-4));
        if (!f.has_file("File")) return reply(uploadFailure(-4));
        const auto file = f.get_file("File");
        long long offset = 0, total = 0;
        try {
            offset = std::stoll(f.get_field("Offset"));
            total = std::stoll(f.get_field("TotalSize"));
        } catch (...) {
            return reply(uploadFailure(-4));
        }
        if (offset < 0) return reply(uploadFailure(-1));
        bool completed = false;
        const json result = [&]() -> json {
            std::lock_guard lock(mu_);
            ++chunks_;
            auto& t = transfers_[f.get_field("Uuid")];
            if (offset == 0) {
                t = Transfer{file.filename, f.get_field("S-File-MD5"), std::uint64_t(total), {}};
            }
            if (std::uint64_t(offset) != t.data.size() || t.name != file.filename) {
                return uploadFailure(-2);
            }
            std::string chunk = file.content;
            if (cfg_.corruptUploads && !chunk.empty()) chunk[0] = char(chunk[0] ^ 0x5A);
            t.data += chunk;
            verifyUntil_ = Clock::now() + std::chrono::hours(1);  // transferring
            if (t.data.size() >= t.total) {
                const std::string uuid = f.get_field("Uuid");
                Transfer done = std::move(t);
                transfers_.erase(uuid);
                verifyUntil_ = Clock::now() + cfg_.verifyTime;
                completed = true;
                if (f.get_field("Check") == "1" && Md5::hex(done.data) != done.md5) {
                    verifyUntil_ = {};
                    return json{{"code", "111111"},
                                {"messages", json::array({{{"field", "S-File-MD5"},
                                                           {"message", "md5 check failed"}}})},
                                {"data", nullptr},
                                {"success", false}};
                }
                files_["/local/" + done.name] = std::move(done.data);
            }
            return uploadSuccess();
        }();
        // Pushes and drops happen outside mu_, so a blocked push stalls only
        // this upload, as it would on a single-threaded mainboard.
        if (cfg_.pushPaddingPerChunk > 0) {
            json push = [&] {
                std::lock_guard lock(mu_);
                return envelope("sdcp/status", json{{"Status", statusObject()}});
            }();
            push["Padding"] = std::string(cfg_.pushPaddingPerChunk, '.');
            const std::string text = push.dump();
            std::lock_guard l(wsMu_);
            for (void* w : sockets_) static_cast<httplib::ws::WebSocket*>(w)->send(text);
        }
        if (completed && cfg_.dropWebSocketsAfterUpload) ++dropGeneration_;
        reply(result);
    });

    // ---- WebSocket control ----
    svr.WebSocket(std::string(kWebSocketPath),
                  [this](const httplib::Request&, httplib::ws::WebSocket& ws) {
                      ++wsConnections_;
                      const int generation = dropGeneration_;
                      {
                          std::lock_guard l(wsMu_);
                          sockets_.push_back(&ws);
                      }
                      ws.set_read_timeout(std::chrono::milliseconds(50));
                      std::string msg;
                      while (!stop_ && dropGeneration_ == generation) {
                          const auto r = ws.read(msg);
                          if (r == httplib::ws::Timeout) continue;
                          if (r == httplib::ws::Fail) break;
                          if (msg == "ping") {
                              ws.send(std::string("pong"));
                              continue;
                          }
                          json req = json::parse(msg, nullptr, false);
                          if (req.is_discarded()) continue;
                          std::vector<json> pushes;
                          json resp = handleCommand(req, pushes);
                          if (resp.is_object() && resp.contains("Drop")) break;  // no answer
                          if (!resp.is_null()) ws.send(resp.dump());
                          for (const auto& p : pushes) ws.send(p.dump());
                      }
                      std::lock_guard l(wsMu_);
                      sockets_.erase(std::find(sockets_.begin(), sockets_.end(), &ws));
                      // Returning drops the connection without a close handshake.
                  });

    const int port = svr.bind_to_any_port("127.0.0.1");
    if (port <= 0) throw std::runtime_error("mock: http bind failed");
    controlPort_ = std::uint16_t(port);
    httpThread_ = std::thread([this] { http_->server.listen_after_bind(); });
    http_->server.wait_until_ready();
    udpThread_ = std::thread([this] { udpLoop(); });
}

MockPrinter::~MockPrinter() {
    stop_ = true;
    http_->server.stop();
    if (httpThread_.joinable()) httpThread_.join();
    if (udpThread_.joinable()) udpThread_.join();
    if (udpFd_ >= 0) ::close(udpFd_);
}

void MockPrinter::udpLoop() {
    while (!stop_) {
        pollfd p{udpFd_, POLLIN, 0};
        if (::poll(&p, 1, 50) <= 0) continue;
        char buf[256];
        sockaddr_in src{};
        socklen_t len = sizeof src;
        const ssize_t n = ::recvfrom(udpFd_, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&src), &len);
        if (n <= 0 || std::string(buf, std::size_t(n)) != kDiscoveryMessage) continue;
        json fields{{"Name", cfg_.name},
                    {"MachineName", cfg_.machineName},
                    {"BrandName", "ELEGOO"},
                    {"MainboardIP", "127.0.0.1"},
                    {"MainboardID", cfg_.mainboardId},
                    {"ProtocolVersion", "V3.0.0"},
                    {"FirmwareVersion", cfg_.firmware}};
        json data = fields;
        if (cfg_.nestedDiscovery) {
            std::lock_guard lock(mu_);
            data = json{{"Attributes", fields}, {"Status", statusObject()}};
        }
        json reply{{"Id", cfg_.id}, {"Data", data}};
        const std::string s = reply.dump();
        ::sendto(udpFd_, s.data(), s.size(), 0, reinterpret_cast<sockaddr*>(&src), len);
    }
}

json MockPrinter::envelope(const std::string& topic, json body) const {
    body["MainboardID"] = cfg_.mainboardId;
    body["TimeStamp"] = unixNow();
    body["Topic"] = topic + "/" + cfg_.mainboardId;
    return body;
}

std::vector<int> MockPrinter::currentStatus() {
    // mu_ held by caller
    if (printing_) return {int(MachineStatus::Printing)};
    if (!transfers_.empty() || Clock::now() < verifyUntil_) return {int(MachineStatus::FileTransferring)};
    return {int(MachineStatus::Idle)};
}

json MockPrinter::statusObject() {
    // mu_ held by caller
    json pi{{"Status", printing_ ? (paused_ ? 6 : 3) : 0},
            {"CurrentLayer", printing_ ? 12 : 0},
            {"TotalLayer", printing_ ? 1000 : 0},
            {"CurrentTicks", 0},
            {"TotalTicks", 0},
            {"Filename", printing_ ? *printing_ : ""},
            {"ErrorNumber", 0},
            {"TaskId", ""}};
    return json{{"CurrentStatus", currentStatus()},
                {"PreviousStatus", 0},
                {"TempOfUVLED", 31.5},
                {"TempOfBox", 26},
                {"ReleaseFilm", 4242},
                {"PrintInfo", pi}};
}

json MockPrinter::attributesObject() const {
    return json{{"Name", cfg_.name},
                {"MachineName", cfg_.machineName},
                {"BrandName", "ELEGOO"},
                {"ProtocolVersion", "V3.0.0"},
                {"FirmwareVersion", cfg_.firmware},
                {"Resolution", "15120x6230"},
                {"MainboardIP", "127.0.0.1"},
                {"MainboardID", cfg_.mainboardId},
                {"SupportFileType", json::array({"CTB", "GOO"})}};
}

json MockPrinter::handleCommand(const json& req, std::vector<json>& pushes) {
    std::lock_guard lock(mu_);
    const bool envelopeOk =
        req.is_object() && req.value("Id", std::string()) == cfg_.id &&
        req.value("Topic", std::string()) == "sdcp/request/" + cfg_.mainboardId &&
        req.contains("Data") && req["Data"].is_object() &&
        req["Data"].value("MainboardID", std::string()) == cfg_.mainboardId &&
        req["Data"].contains("RequestID") && req["Data"]["RequestID"].is_string() &&
        req["Data"].contains("Cmd") && req["Data"]["Cmd"].is_number_integer() &&
        req["Data"].contains("TimeStamp") && req["Data"].contains("From");
    if (!envelopeOk) {
        ++rejected_;
        return nullptr;  // real firmware behaviour unknown; stay silent
    }
    const json& d = req["Data"];
    const int cmd = d["Cmd"].get<int>();
    const json args = d.contains("Data") && d["Data"].is_object() ? d["Data"] : json::object();
    commands_.push_back(cmd);

    json data{{"Ack", 0}};
    switch (cmd) {
        case 0:
            pushes.push_back(envelope("sdcp/status", json{{"Status", statusObject()}}));
            break;
        case 1:
            pushes.push_back(envelope("sdcp/attributes", json{{"Attributes", attributesObject()}}));
            break;
        case 128: {
            ++startRequests_;
            const std::string name = args.value("Filename", std::string());
            if (cfg_.dropOnStart != MockPrinterConfig::StartDrop::None) {
                if (cfg_.dropOnStart == MockPrinterConfig::StartDrop::AfterStarting && files_.count(name))
                    printing_ = name;
                cfg_.dropOnStart = MockPrinterConfig::StartDrop::None;  // once
                return json{{"Drop", true}};
            }
            if (cfg_.forcedStartAck) data["Ack"] = *cfg_.forcedStartAck;
            else if (printing_ || !transfers_.empty() ||
                     Clock::now() < verifyUntil_ + cfg_.extraBusyTime)
                data["Ack"] = 1;
            else if (!files_.count(name)) data["Ack"] = 2;
            else printing_ = name;
            if (data["Ack"] == 1) ++busyAnswers_;
            break;
        }
        case 129: paused_ = true; break;
        case 130: printing_.reset(); paused_ = false; break;
        case 131: paused_ = false; break;
        case 132: case 133: case 260:
            return nullptr;  // silent on the 16K
        case 255: {
            ++cancelRequests_;
            const auto it = transfers_.find(args.value("Uuid", std::string()));
            if (it == transfers_.end()) data["Ack"] = 1;
            else {
                transfers_.erase(it);
                verifyUntil_ = {};
            }
            break;
        }
        case 258: {
            std::string url = args.value("Url", std::string("/local"));
            if (url.empty() || url.front() != '/') url = "/local/" + url;
            if (url.back() != '/') url += '/';
            json list = json::array();
            for (const auto& [path, content] : files_)
                if (path.rfind(url, 0) == 0) list.push_back({{"name", path}, {"type", 1}});
            data["FileList"] = list;
            break;
        }
        case 259:
            if (args.contains("FileList") && args["FileList"].is_array())
                for (const auto& f : args["FileList"])
                    if (f.is_string()) files_.erase(f.get<std::string>());
            break;  // Ack 0 even for missing files, as on the device
        default:
            return nullptr;
    }
    json resp{{"Id", cfg_.id},
              {"Data",
               {{"Cmd", cmd},
                {"Data", data},
                {"RequestID", d["RequestID"]},
                {"MainboardID", cfg_.mainboardId},
                {"TimeStamp", unixNow()}}},
              {"Topic", "sdcp/response/" + cfg_.mainboardId}};
    return resp;
}

std::map<std::string, std::string> MockPrinter::files() const {
    std::lock_guard lock(mu_);
    return files_;
}
int MockPrinter::startRequests() const { std::lock_guard l(mu_); return startRequests_; }
int MockPrinter::busyAnswers() const { std::lock_guard l(mu_); return busyAnswers_; }
int MockPrinter::cancelRequests() const { std::lock_guard l(mu_); return cancelRequests_; }
int MockPrinter::chunksReceived() const { std::lock_guard l(mu_); return chunks_; }
int MockPrinter::rejectedRequests() const { std::lock_guard l(mu_); return rejected_; }
int MockPrinter::webSocketConnections() const { return wsConnections_; }
std::optional<std::string> MockPrinter::printing() const { std::lock_guard l(mu_); return printing_; }
std::vector<int> MockPrinter::commandsSeen() const { std::lock_guard l(mu_); return commands_; }
void MockPrinter::addFile(const std::string& p, const std::string& c) { std::lock_guard l(mu_); files_[p] = c; }
void MockPrinter::setPrinting(const std::string& p) { std::lock_guard l(mu_); printing_ = p; }

}  // namespace fruit::net::testing
