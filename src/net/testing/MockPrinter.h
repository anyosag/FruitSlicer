// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// An in-process SDCP V3 printer for tests: UDP discovery responder, HTTP
// chunked upload endpoint and WebSocket control channel, all on loopback
// with ephemeral ports. It models the documented behaviour (docs/wifi-sdcp.md)
// including the Saturn 4 Ultra 16K's quirks: Cmd 128 answers "busy" for a
// while after an upload even though the file is already listed, and
// Cmds 132/133/260 never answer.

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace fruit::net::testing {

struct MockPrinterConfig {
    std::string id = "0123456789abcdef0123456789abcdef";
    std::string mainboardId = "000000000001d354";
    std::string name = "Mock Saturn";
    std::string machineName = "Saturn 4 Ultra 16K";
    std::string firmware = "V1.5.6";
    // Nest discovery fields under Data.Attributes (seen on some firmware).
    bool nestedDiscovery = false;
    // How long the printer stays in "file transferring" (verifying) after the
    // last chunk; Cmd 128 answers Ack 1 (busy) during that time.
    std::chrono::milliseconds verifyTime{300};
    // Cmd 128 keeps answering busy this long after verification ends (the
    // 16K's busy-after-upload race); CurrentStatus already reads idle.
    std::chrono::milliseconds extraBusyTime{0};
    // Flip a byte of every stored chunk, so the MD5 check fails.
    bool corruptUploads = false;
    // Force this Ack for every Cmd 128 (to exercise error messages).
    std::optional<int> forcedStartAck;
};

class MockPrinter {
public:
    explicit MockPrinter(MockPrinterConfig config = {});
    ~MockPrinter();
    MockPrinter(const MockPrinter&) = delete;
    MockPrinter& operator=(const MockPrinter&) = delete;

    std::uint16_t udpPort() const { return udpPort_; }
    std::uint16_t controlPort() const { return controlPort_; }

    // Inspection (thread-safe).
    std::map<std::string, std::string> files() const;  // path -> content
    int startRequests() const;
    int busyAnswers() const;
    int cancelRequests() const;
    int chunksReceived() const;
    int rejectedRequests() const;  // bad envelope (wrong Topic/MainboardID/Id)
    std::optional<std::string> printing() const;  // file being printed
    std::vector<int> commandsSeen() const;
    void addFile(const std::string& path, const std::string& content);
    void setPrinting(const std::string& path);

private:
    struct Transfer {
        std::string name;
        std::string md5;
        std::uint64_t total = 0;
        std::string data;
    };

    void udpLoop();
    nlohmann::json handleCommand(const nlohmann::json& req, std::vector<nlohmann::json>& pushes);
    nlohmann::json statusObject();
    nlohmann::json attributesObject() const;
    nlohmann::json envelope(const std::string& topic, nlohmann::json body) const;
    std::vector<int> currentStatus();

    MockPrinterConfig cfg_;
    mutable std::mutex mu_;
    std::map<std::string, std::string> files_;
    std::map<std::string, Transfer> transfers_;
    std::chrono::steady_clock::time_point verifyUntil_{};
    std::optional<std::string> printing_;
    bool paused_ = false;
    int startRequests_ = 0, busyAnswers_ = 0, cancelRequests_ = 0, chunks_ = 0, rejected_ = 0;
    std::vector<int> commands_;

    struct Http;
    std::unique_ptr<Http> http_;
    std::thread httpThread_;
    std::thread udpThread_;
    int udpFd_ = -1;
    std::atomic<bool> stop_{false};
    std::uint16_t udpPort_ = 0;
    std::uint16_t controlPort_ = 0;
};

}  // namespace fruit::net::testing
