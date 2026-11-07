// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Send.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "net/Discovery.h"

namespace gui {

namespace net = fruit::net;

std::vector<net::PrinterInfo> findPrinters(const std::vector<std::string>& hosts, const NetPorts& ports,
                                           const fruit::JobControl& ctl) {
    ctl.step("search", 0);
    net::DiscoveryOptions o;
    o.port = ports.udp;
    o.broadcast = ports.broadcast;
    o.unicastTargets = hosts;
    o.timeout = std::chrono::milliseconds(ports.broadcast ? 2500 : 1200);
    std::vector<net::PrinterInfo> found = net::discover(o);
    ctl.step("search", 1);
    std::sort(found.begin(), found.end(), [](const net::PrinterInfo& a, const net::PrinterInfo& b) {
        return printerName(a) != printerName(b) ? printerName(a) < printerName(b) : a.ip < b.ip;
    });
    return found;
}

net::PrinterInfo probePrinter(const std::string& host, const NetPorts& ports) {
    try {
        return net::probe(host, ports.udp);
    } catch (const net::TimeoutError&) {
        throw std::runtime_error("Nothing answered at " + host +
                                 ". Check the address on the printer's network screen, and that it is on.");
    }
}

SendResult sendToPrinter(const net::PrinterInfo& printer, const std::string& path, bool start, const NetPorts& ports,
                         const fruit::JobControl& ctl) {
    net::ClientOptions co;
    co.udpPort = ports.udp;
    co.controlPort = ports.control;
    SendResult r;
    r.printer = printerName(printer);
    const double upload_end = start ? 0.9 : 1.0;
    try {
        ctl.step("connect", 0);
        net::SdcpClient client(printer, co);
        r.upload = client.upload(
            path,
            [&](const net::UploadProgress& p) {
                if (p.total) ctl.report("send", upload_end * static_cast<double>(p.sent) / static_cast<double>(p.total));
            },
            ctl.cancelFlag());
        if (start) {
            ctl.step("start", upload_end);
            client.startPrint(r.upload.remotePath, net::StartOptions{}, ctl.cancelFlag());
            r.started = true;
        }
        ctl.report(start ? "start" : "send", 1.0);
    } catch (const net::CancelledError&) {
        throw fruit::Cancelled();
    }
    return r;
}

std::string printerName(const net::PrinterInfo& p) {
    if (!p.name.empty()) return p.name;
    if (!p.machineName.empty()) return p.machineName;
    return p.ip;
}

std::vector<std::string> loadPrinterAddresses(const std::string& file) {
    std::ifstream in(file);
    if (!in) return {};
    try {
        const nlohmann::json j = nlohmann::json::parse(in);
        std::vector<std::string> hosts;
        for (const auto& h : j.value("addresses", nlohmann::json::array()))
            if (h.is_string() && !h.get<std::string>().empty()) hosts.push_back(h.get<std::string>());
        return hosts;
    } catch (const std::exception&) {
        return {};
    }
}

void savePrinterAddresses(const std::string& file, const std::vector<std::string>& hosts) {
    const std::filesystem::path p(file);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    const std::string tmp = file + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + tmp);
        out << nlohmann::json{{"addresses", hosts}}.dump(2) << "\n";
        if (!out) throw std::runtime_error("cannot write " + tmp);
    }
    std::filesystem::rename(tmp, file);
}

}  // namespace gui
