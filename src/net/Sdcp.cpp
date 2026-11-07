// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "net/Sdcp.h"

#include <algorithm>

namespace fruit::net {

using nlohmann::json;

namespace {

std::string str(const json& obj, const char* key) {
    if (!obj.is_object()) return {};
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return {};
    if (it->is_string()) return it->get<std::string>();
    return it->dump();
}

std::optional<double> num(const json& obj, const char* key) {
    if (!obj.is_object()) return std::nullopt;
    auto it = obj.find(key);
    if (it == obj.end()) return std::nullopt;
    if (it->is_number()) return it->get<double>();
    if (it->is_string()) {
        try {
            return std::stod(it->get<std::string>());
        } catch (...) {
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::int64_t> jsonInt(const json& obj, const char* key) {
    auto v = num(obj, key);
    if (!v) return std::nullopt;
    return std::int64_t(*v);
}

bool PrinterStatus::has(MachineStatus s) const {
    return std::find(current.begin(), current.end(), int(s)) != current.end();
}

std::optional<PrinterInfo> parseDiscoveryReply(std::string_view datagram,
                                               const std::string& senderIp) {
    json j = json::parse(datagram.begin(), datagram.end(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;
    const json data = j.contains("Data") && j["Data"].is_object() ? j["Data"] : j;
    const json attrs =
        data.contains("Attributes") && data["Attributes"].is_object() ? data["Attributes"] : data;
    PrinterInfo p;
    p.id = str(j, "Id");
    p.mainboardId = str(attrs, "MainboardID");
    if (p.mainboardId.empty()) p.mainboardId = str(data, "MainboardID");
    if (p.mainboardId.empty()) return std::nullopt;  // not an SDCP reply we can talk to
    p.ip = str(attrs, "MainboardIP");
    if (p.ip.empty()) p.ip = senderIp;
    p.name = str(attrs, "Name");
    p.machineName = str(attrs, "MachineName");
    p.brandName = str(attrs, "BrandName");
    p.protocolVersion = str(attrs, "ProtocolVersion");
    p.firmwareVersion = str(attrs, "FirmwareVersion");
    p.raw = std::move(j);
    return p;
}

json makeRequest(const PrinterInfo& printer, int cmd, const json& data,
                 const std::string& requestId, std::int64_t timestamp) {
    return json{{"Id", printer.id},
                {"Data",
                 {{"Cmd", cmd},
                  {"Data", data.is_null() ? json::object() : data},
                  {"RequestID", requestId},
                  {"MainboardID", printer.mainboardId},
                  {"TimeStamp", timestamp},
                  {"From", 0}}},
                {"Topic", "sdcp/request/" + printer.mainboardId}};
}

PrinterStatus parseStatus(const json& s) {
    PrinterStatus st;
    st.raw = s;
    if (!s.is_object()) return st;
    if (auto it = s.find("CurrentStatus"); it != s.end()) {
        if (it->is_array()) {
            for (const auto& v : *it)
                if (v.is_number()) st.current.push_back(v.get<int>());
        } else if (it->is_number()) {
            st.current.push_back(it->get<int>());
        }
    }
    st.previous = int(jsonInt(s, "PreviousStatus").value_or(0));
    st.uvLedTemp = num(s, "TempOfUVLED");
    st.boxTemp = num(s, "TempOfBox");
    st.releaseFilm = jsonInt(s, "ReleaseFilm");
    if (auto it = s.find("PrintInfo"); it != s.end() && it->is_object()) {
        const json& p = *it;
        PrintInfo pi;
        pi.status = int(jsonInt(p, "Status").value_or(0));
        pi.currentLayer = int(jsonInt(p, "CurrentLayer").value_or(0));
        pi.totalLayer = int(jsonInt(p, "TotalLayer").value_or(0));
        pi.currentTicksMs = jsonInt(p, "CurrentTicks").value_or(0);
        pi.totalTicksMs = jsonInt(p, "TotalTicks").value_or(0);
        pi.filename = str(p, "Filename");
        pi.errorNumber = int(jsonInt(p, "ErrorNumber").value_or(0));
        pi.taskId = str(p, "TaskId");
        st.print = pi;
    }
    return st;
}

std::vector<FileEntry> parseFileList(const json& d) {
    std::vector<FileEntry> out;
    if (!d.is_object() || !d.contains("FileList") || !d["FileList"].is_array()) return out;
    for (const auto& f : d["FileList"]) {
        if (!f.is_object()) continue;
        FileEntry e;
        e.name = str(f, "name");
        e.isFolder = jsonInt(f, "type").value_or(1) == 0;  // spec: 0 folder, 1 file
        e.usedSize = jsonInt(f, "usedSize");
        e.totalSize = jsonInt(f, "totalSize");
        if (auto t = jsonInt(f, "storageType")) e.storageType = int(*t);
        out.push_back(std::move(e));
    }
    return out;
}

std::string machineStatusName(int c) {
    switch (c) {
        case 0: return "idle";
        case 1: return "printing";
        case 2: return "file transferring";
        case 3: return "exposure test";
        case 4: return "device self-check";
    }
    return "status " + std::to_string(c);
}

std::string printStatusName(int c) {
    static const char* names[] = {"idle",     "homing", "dropping", "exposing",
                                  "lifting",  "pausing", "paused",  "stopping",
                                  "stopped",  "complete", "file checking"};
    if (c >= 0 && c <= 10) return names[c];
    return "print status " + std::to_string(c);
}

std::string printErrorName(int c) {
    switch (c) {
        case 0: return "none";
        case 1: return "file MD5 check failed";
        case 2: return "file read failed";
        case 3: return "resolution mismatch";
        case 4: return "file format mismatch";
        case 5: return "machine model mismatch";
    }
    return "error " + std::to_string(c);
}

std::string startPrintAckName(int a) {
    switch (a) {
        case 0: return "OK";
        case 1: return "printer busy";
        case 2: return "file not found on the printer";
        case 3: return "MD5 verification failed (file corrupted in transfer)";
        case 4: return "printer could not read the file";
        case 5: return "resolution mismatch (file was sliced for another screen)";
        case 6: return "unrecognised file format";
        case 7: return "machine model mismatch (file was sliced for another printer)";
    }
    return "unknown Ack " + std::to_string(a);
}

std::string cancelTransferAckName(int a) {
    switch (a) {
        case 0: return "OK";
        case 1: return "printer is not transferring a file";
        case 2: return "printer is already verifying the file";
        case 3: return "file not found";
    }
    return "unknown Ack " + std::to_string(a);
}

std::string uploadErrorName(int c) {
    switch (c) {
        case -1: return "offset error (negative offset)";
        case -2: return "offset does not match the file on the printer";
        case -3: return "printer could not open the file";
        case -4: return "unknown error";
    }
    return "error " + std::to_string(c);
}

}  // namespace fruit::net
