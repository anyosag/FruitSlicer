// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// SDCP V3 (Smart Device Control Protocol, CBD/Chitu) message types and the
// pure encode/decode helpers shared by the client and the mock printer.
//
// Implemented from the published spec
// (github.com/cbd-tech/SDCP-Smart-Device-Control-Protocol-V3.0.0) plus
// real-device notes for the Saturn 4 Ultra 16K; see docs/wifi-sdcp.md for
// what is verified and what is not.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fruit::net {

// Default ports (spec: UDP 3000 discovery; WebSocket and HTTP upload on 3030).
inline constexpr std::uint16_t kDiscoveryPort = 3000;
inline constexpr std::uint16_t kControlPort = 3030;
inline constexpr std::string_view kDiscoveryMessage = "M99999";
inline constexpr std::string_view kWebSocketPath = "/websocket";
inline constexpr std::string_view kUploadPath = "/uploadFile/upload";
inline constexpr std::size_t kUploadChunkSize = 1024 * 1024;

// Commands. Camera/video (386) and time-lapse (387) are deliberately absent:
// on the 16K leaked video sessions survive until the printer is power-cycled.
enum class Cmd : int {
    Status = 0,
    Attributes = 1,
    StartPrint = 128,
    Pause = 129,
    Stop = 130,
    Resume = 131,
    CancelTransfer = 255,
    FileList = 258,
    DeleteFiles = 259,
};

// sdcp_machine_status_t (CurrentStatus / PreviousStatus).
enum class MachineStatus : int {
    Idle = 0,
    Printing = 1,
    FileTransferring = 2,
    ExposureTesting = 3,
    DevicesTesting = 4,
};

// ---- errors ---------------------------------------------------------------

class NetError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// No answer in time (UDP probe, WebSocket response, status push).
class TimeoutError : public NetError {
public:
    using NetError::NetError;
};

// The printer answered a command with a non-zero Ack.
class AckError : public NetError {
public:
    AckError(int cmd, int ack, const std::string& what)
        : NetError(what), cmd_(cmd), ack_(ack) {}
    int cmd() const { return cmd_; }
    int ack() const { return ack_; }

private:
    int cmd_;
    int ack_;
};

// The HTTP upload endpoint refused a chunk or the transfer failed.
class UploadError : public NetError {
public:
    using NetError::NetError;
};

// The user asked to cancel (std::atomic<bool> flag).
class CancelledError : public NetError {
public:
    using NetError::NetError;
};

// ---- data -----------------------------------------------------------------

// The discovery reply (spec: {Id, Data{Name, MachineName, BrandName,
// MainboardIP, MainboardID, ProtocolVersion, FirmwareVersion}}; some
// firmware nests those under Data.Attributes, both are accepted).
struct PrinterInfo {
    std::string id;              // "Id": sent back in every request
    std::string mainboardId;     // "MainboardID": used in every topic
    std::string ip;              // MainboardIP, or the UDP sender address
    std::string name;
    std::string machineName;
    std::string brandName;
    std::string protocolVersion;
    std::string firmwareVersion;
    nlohmann::json raw;
};

struct PrintInfo {
    int status = 0;  // sdcp_print_status_t
    int currentLayer = 0;
    int totalLayer = 0;
    std::int64_t currentTicksMs = 0;
    std::int64_t totalTicksMs = 0;
    std::string filename;
    int errorNumber = 0;
    std::string taskId;
};

struct PrinterStatus {
    std::vector<int> current;  // CurrentStatus (array per spec; an int is accepted)
    int previous = 0;
    std::optional<double> uvLedTemp;
    std::optional<double> boxTemp;
    std::optional<std::int64_t> releaseFilm;
    std::optional<PrintInfo> print;
    nlohmann::json raw;  // the "Status" object

    bool has(MachineStatus s) const;
    bool idle() const { return current.empty() || (current.size() == 1 && current[0] == 0); }
};

struct FileEntry {
    std::string name;  // full path, e.g. "/local/part.goo"
    bool isFolder = false;
    std::optional<std::int64_t> usedSize;
    std::optional<std::int64_t> totalSize;
    std::optional<int> storageType;
};

// ---- encode / decode --------------------------------------------------------

// Parses a discovery datagram; nullopt if it is not an SDCP reply.
std::optional<PrinterInfo> parseDiscoveryReply(std::string_view datagram,
                                               const std::string& senderIp);

nlohmann::json makeRequest(const PrinterInfo& printer, int cmd, const nlohmann::json& data,
                           const std::string& requestId, std::int64_t timestamp);

PrinterStatus parseStatus(const nlohmann::json& statusObject);
std::vector<FileEntry> parseFileList(const nlohmann::json& responseData);

// Names for codes, for messages.
std::string machineStatusName(int code);
std::string printStatusName(int code);
std::string printErrorName(int code);       // PrintInfo.ErrorNumber
std::string startPrintAckName(int ack);     // Cmd 128 Ack
std::string cancelTransferAckName(int ack); // Cmd 255 Ack
std::string uploadErrorName(int code);      // HTTP "common_field" codes

// Reads a JSON member that firmware may send as a number or a string.
std::optional<std::int64_t> jsonInt(const nlohmann::json& obj, const char* key);

}  // namespace fruit::net
