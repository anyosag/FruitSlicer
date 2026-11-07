// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Sending the print file to the printer over Wi-Fi (SDCP V3, src/net): find
// printers, upload with progress and cancel, start the print only when
// asked. Worker-thread work; no UI here.

#include <cstdint>
#include <string>
#include <vector>

#include "core/JobControl.h"
#include "net/Sdcp.h"
#include "net/SdcpClient.h"

namespace gui {

// Where printers listen. Scripts point these at a mock printer.
struct NetPorts {
    std::uint16_t udp = fruit::net::kDiscoveryPort;
    std::uint16_t control = fruit::net::kControlPort;
    bool broadcast = true;  // ask the whole network, not only the known addresses
};

// Printers that answer within a couple of seconds: the whole network (when
// `ports.broadcast`) and each of `hosts` asked directly. One entry per
// printer, by its mainboard id. Progress stage "search".
std::vector<fruit::net::PrinterInfo> findPrinters(const std::vector<std::string>& hosts, const NetPorts& ports,
                                                  const fruit::JobControl& ctl);

// Asks one address directly; throws with a readable message if nothing answers.
fruit::net::PrinterInfo probePrinter(const std::string& host, const NetPorts& ports);

struct SendResult {
    fruit::net::UploadResult upload;
    std::string printer;  // its name, for the result line
    bool started = false;  // the print was started
};

// Uploads `path` (progress stage "send", 0..0.9 when starting, else to 1),
// then starts the print when `start` (stage "start"). Cancel stops the
// upload and tells the printer to drop the partial file.
SendResult sendToPrinter(const fruit::net::PrinterInfo& printer, const std::string& path, bool start,
                         const NetPorts& ports, const fruit::JobControl& ctl);

// A printer's name as people know it: its own name, else its model.
std::string printerName(const fruit::net::PrinterInfo& p);

// Addresses added by hand, kept between launches. Missing or unreadable
// files read as none; writing creates the folder.
std::vector<std::string> loadPrinterAddresses(const std::string& file);
void savePrinterAddresses(const std::string& file, const std::vector<std::string>& hosts);

}  // namespace gui
