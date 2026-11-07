// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "net/cli/PrinterCli.h"

#include "net/Discovery.h"
#include "net/SdcpClient.h"

#include <CLI/CLI.hpp>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace fruit::net {

using nlohmann::json;

namespace {

json toJson(const PrinterInfo& p) {
    return json{{"ip", p.ip},
                {"name", p.name},
                {"machine", p.machineName},
                {"brand", p.brandName},
                {"mainboardId", p.mainboardId},
                {"id", p.id},
                {"protocol", p.protocolVersion},
                {"firmware", p.firmwareVersion}};
}

std::string minutes(std::int64_t ms) {
    std::ostringstream s;
    s << ms / 60000 << " min";
    return s.str();
}

void printStatus(std::ostream& out, const PrinterStatus& st) {
    std::string states;
    for (int c : st.current) states += (states.empty() ? "" : ", ") + machineStatusName(c);
    out << "state:    " << (states.empty() ? "idle" : states) << "\n";
    if (st.print) {
        const auto& p = *st.print;
        out << "print:    " << printStatusName(p.status);
        if (!p.filename.empty()) out << "  (" << p.filename << ")";
        out << "\n";
        if (p.totalLayer > 0)
            out << "layer:    " << p.currentLayer << " / " << p.totalLayer << "  (" << std::fixed
                << std::setprecision(1) << 100.0 * p.currentLayer / p.totalLayer << "%)\n";
        if (p.totalTicksMs > 0)
            out << "time:     " << minutes(p.currentTicksMs) << " / " << minutes(p.totalTicksMs) << "\n";
        if (p.errorNumber) out << "error:    " << printErrorName(p.errorNumber) << "\n";
    }
    out << std::defaultfloat;
    if (st.uvLedTemp) out << "UV LED:   " << *st.uvLedTemp << " °C\n";
    if (st.boxTemp) out << "enclosure:" << " " << *st.boxTemp << " °C\n";
    if (st.releaseFilm) out << "film:     " << *st.releaseFilm << " releases\n";
}

std::string humanBytes(double b) {
    std::ostringstream s;
    s << std::fixed << std::setprecision(1);
    if (b >= 1024 * 1024) s << b / (1024 * 1024) << " MB";
    else s << b / 1024 << " KB";
    return s.str();
}

}  // namespace

int runPrinterCli(int argc, const char* const* argv, std::ostream& out, std::ostream& err,
                  std::istream& in, const std::atomic<bool>* cancel) {
    CLI::App app{"fruitslicer-printer: send .goo files to an ELEGOO SDCP V3 printer over Wi-Fi.\n"
                 "No authentication exists in SDCP: use it on a network you trust."};
    app.require_subcommand(1);
    app.fallthrough();  // allow "status <ip> --json" as well as "--json status <ip>"
    bool midLine = false;  // a progress line is open on stderr
    bool asJson = false;
    int udpPort = kDiscoveryPort, controlPort = kControlPort;
    app.add_flag("--json", asJson, "Machine-readable output on stdout");
    app.add_option("--udp-port", udpPort, "Discovery port (default 3000)")->group("Advanced");
    app.add_option("--port", controlPort, "WebSocket/upload port (default 3030)")->group("Advanced");

    auto* cDiscover = app.add_subcommand("discover", "Find printers on the local network");
    int timeoutMs = 2500;
    cDiscover->add_option("--timeout", timeoutMs, "How long to listen for answers, ms");
    std::vector<std::string> ask;
    cDiscover->add_option("--ask", ask, "Also ask this address directly (if broadcasts are blocked)");
    bool noBroadcast = false;
    cDiscover->add_flag("--no-broadcast", noBroadcast, "Only ask the --ask addresses");

    std::string host;
    auto* cStatus = app.add_subcommand("status", "Show the printer's state");
    cStatus->add_option("ip", host, "Printer address")->required();

    auto* cFiles = app.add_subcommand("files", "List files stored on the printer");
    cFiles->add_option("ip", host, "Printer address")->required();
    std::string dir = "/local";
    cFiles->add_option("--dir", dir, "Storage to list: /local (default) or /usb");

    auto* cSend = app.add_subcommand("send", "Upload a .goo file; with --start, also print it");
    cSend->add_option("ip", host, "Printer address")->required();
    std::string file;
    cSend->add_option("file", file, "File to upload (.goo)")->required()->check(CLI::ExistingFile);
    bool start = false, yes = false;
    int startLayer = 0;
    cSend->add_flag("--start", start, "Start printing after the upload (asks for confirmation)");
    cSend->add_flag("--yes,-y", yes, "Do not ask before starting the print");
    cSend->add_option("--start-layer", startLayer, "Layer to start from (default 0)")
        ->check(CLI::NonNegativeNumber);

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e, out, err);
    }

    ClientOptions co;
    co.udpPort = std::uint16_t(udpPort);
    co.controlPort = std::uint16_t(controlPort);

    try {
        if (*cDiscover) {
            DiscoveryOptions o;
            o.timeout = std::chrono::milliseconds(timeoutMs);
            o.port = std::uint16_t(udpPort);
            o.unicastTargets = ask;
            o.broadcast = !noBroadcast;
            const auto found = discover(o);
            if (asJson) {
                json a = json::array();
                for (const auto& p : found) a.push_back(toJson(p));
                out << a.dump(2) << "\n";
            } else if (found.empty()) {
                err << "No SDCP printer answered. Is it on, and on the same network as this computer?\n";
            } else {
                for (const auto& p : found)
                    out << p.ip << "  " << p.name << "  (" << p.brandName << " " << p.machineName
                        << ", firmware " << p.firmwareVersion << ", SDCP " << p.protocolVersion
                        << ")\n";
            }
            return found.empty() ? 2 : 0;
        }

        SdcpClient client(host, co);
        const auto& printer = client.printer();

        if (*cStatus) {
            const auto st = client.status();
            if (asJson) {
                out << json{{"printer", toJson(printer)}, {"status", st.raw}}.dump(2) << "\n";
            } else {
                out << printer.name << " (" << printer.machineName << ") at " << host << "\n";
                printStatus(out, st);
            }
            return 0;
        }

        if (*cFiles) {
            const auto list = client.files(dir);
            if (asJson) {
                json a = json::array();
                for (const auto& f : list) a.push_back({{"name", f.name}, {"folder", f.isFolder}});
                out << a.dump(2) << "\n";
            } else {
                if (list.empty()) err << "(no files in " << dir << ")\n";
                for (const auto& f : list) out << (f.isFolder ? "dir   " : "file  ") << f.name << "\n";
            }
            return 0;
        }

        if (*cSend) {
            const std::filesystem::path path(file);
            std::string ext = path.extension().string();
            for (auto& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (ext != ".goo" && ext != ".ctb")
                err << "warning: " << path.filename().string()
                    << " is not a .goo/.ctb file; the printer may refuse it\n";

            const auto st = client.status();
            if (start && st.has(MachineStatus::Printing)) {
                err << "The printer is already printing; not uploading with --start. "
                       "Run without --start to only upload.\n";
                return 3;
            }
            if (start && !yes) {
                err << "About to upload " << path.filename().string() << " to " << printer.name
                    << " (" << host << ") and START PRINTING it from layer " << startLayer
                    << ".\nThe build plate will move. Check the vat and plate first.\n"
                    << "Type 'yes' to continue: " << std::flush;
                std::string answer;
                if (!std::getline(in, answer) || answer != "yes") {
                    err << "Not confirmed; nothing was sent.\n";
                    return 4;
                }
            }

            const auto size = std::filesystem::file_size(path);
            err << "Uploading " << path.filename().string() << " (" << humanBytes(double(size))
                << ") to " << host << "\n";
            int lastPct = -1;
            const auto result = client.upload(
                path,
                [&](const UploadProgress& p) {
                    const int pct = int(100 * p.sent / std::max<std::uint64_t>(p.total, 1));
                    if (pct == lastPct) return;
                    lastPct = pct;
                    midLine = true;
                    const double eta = p.bytesPerSecond > 0 ? double(p.total - p.sent) / p.bytesPerSecond : 0;
                    err << "\r  " << std::setw(3) << pct << "%  " << humanBytes(p.bytesPerSecond)
                        << "/s  ~" << int(std::ceil(eta)) << " s left   " << std::flush;
                },
                cancel);
            midLine = false;
            err << "\nUploaded as " << result.remotePath << " in " << std::fixed << std::setprecision(1)
                << result.seconds << " s (MD5 " << result.md5 << ")\n"
                << std::defaultfloat;

            bool started = false;
            if (start) {
                StartOptions so;
                so.startLayer = startLayer;
                so.log = [&](const std::string& s) { err << s << "\n"; };
                client.startPrint(result.remotePath, so, cancel);
                started = true;
                err << "Print started: " << result.remotePath << "\n";
            } else {
                err << "Not started. Start it on the printer's screen, or run again with --start.\n";
            }
            if (asJson)
                out << json{{"printer", toJson(printer)},
                            {"remotePath", result.remotePath},
                            {"size", result.size},
                            {"md5", result.md5},
                            {"seconds", result.seconds},
                            {"started", started}}
                           .dump(2)
                    << "\n";
            return 0;
        }
    } catch (const CancelledError& e) {
        err << (midLine ? "\n" : "") << e.what() << "\n";
        return 130;
    } catch (const std::exception& e) {
        err << (midLine ? "\n" : "") << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

}  // namespace fruit::net
