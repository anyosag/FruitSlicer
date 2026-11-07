// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// A blocking SDCP V3 client for one printer: WebSocket control channel on
// ws://<ip>:3030/websocket and chunked multipart upload to
// http://<ip>:3030/uploadFile/upload.
//
// Programs on Linux should ignore SIGPIPE (macOS sockets are set up not to
// raise it), or a printer dropping a connection ends the process.
//
// Not thread-safe: use one SdcpClient per thread. Errors are exceptions
// derived from NetError (see Sdcp.h). Nothing here touches the camera.

#include "net/Sdcp.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fruit::net {

struct ClientOptions {
    std::uint16_t udpPort = kDiscoveryPort;
    std::uint16_t controlPort = kControlPort;  // WebSocket and HTTP upload
    std::chrono::milliseconds probeTimeout{2000};
    std::chrono::milliseconds connectTimeout{8000};
    std::chrono::milliseconds commandTimeout{8000};
    std::chrono::milliseconds chunkTimeout{60000};  // one 1 MB chunk at ~170 KB/s takes ~6 s
    std::size_t chunkSize = kUploadChunkSize;
};

struct UploadProgress {
    std::uint64_t sent = 0;   // bytes acknowledged by the printer
    std::uint64_t total = 0;
    double bytesPerSecond = 0;
};
using ProgressFn = std::function<void(const UploadProgress&)>;

struct UploadResult {
    std::string remoteName;  // as stored, e.g. "part.goo"
    std::string remotePath;  // what Cmd 128 is given, e.g. "/local/part.goo"
    std::string md5;
    std::string uuid;
    std::uint64_t size = 0;
    double seconds = 0;
};

struct StartOptions {
    int startLayer = 0;
    // After an upload the printer verifies the file (CurrentStatus 2) and
    // answers Cmd 128 with Ack 1 (busy) until it is done.
    std::chrono::milliseconds settleTimeout{std::chrono::minutes(3)};
    std::chrono::milliseconds pollInterval{2000};
    std::chrono::milliseconds busyRetryDelay{5000};
    int busyRetries = 6;
    std::function<void(const std::string&)> log;  // progress messages, optional
};

class SdcpClient {
public:
    // Asks `host` for its identity over UDP (Id and MainboardID are needed in
    // every request), then opens the WebSocket.
    explicit SdcpClient(const std::string& host, ClientOptions options = {});
    // Uses an identity from discover(); no UDP probe.
    SdcpClient(PrinterInfo printer, ClientOptions options = {});
    ~SdcpClient();
    SdcpClient(const SdcpClient&) = delete;
    SdcpClient& operator=(const SdcpClient&) = delete;

    const PrinterInfo& printer() const;

    // Sends a command and returns the response's inner Data object (with
    // "Ack"). Throws TimeoutError if no response arrives; does not check Ack.
    nlohmann::json command(Cmd cmd, const nlohmann::json& data = nlohmann::json::object(),
                           std::chrono::milliseconds timeout = std::chrono::milliseconds(0));

    PrinterStatus status();
    nlohmann::json attributes();  // the "Attributes" object
    std::vector<FileEntry> files(const std::string& url = "/local");

    // Uploads in chunks; `cancel` (optional) is polled during the transfer.
    // On cancel the printer is told to drop the partial file (Cmd 255) and
    // CancelledError is thrown.
    UploadResult upload(const std::filesystem::path& file, const ProgressFn& progress = {},
                        const std::atomic<bool>* cancel = nullptr,
                        const std::string& remoteName = {});

    // Cmd 255. Throws AckError on a non-zero Ack.
    void cancelTransfer(const std::string& uuid, const std::string& fileName);

    // Waits for any file verification to finish, then sends Cmd 128,
    // retrying while the printer answers "busy". Refuses if a print is
    // already running. Throws AckError with a readable message otherwise.
    void startPrint(const std::string& remotePath, const StartOptions& options = {},
                    const std::atomic<bool>* cancel = nullptr);

    // Messages received on sdcp/error/<id> so far.
    const std::vector<nlohmann::json>& errors() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// "/local/<name>" unless the name already is a path.
std::string remotePathFor(const std::string& name);

}  // namespace fruit::net
