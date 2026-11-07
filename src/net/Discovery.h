// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// SDCP discovery: send "M99999" over UDP to port 3000 and collect the JSON
// replies. Broadcasts go to the directed broadcast address of every
// broadcast-capable IPv4 interface (Wi-Fi and Ethernet alike, as Apple's
// TN3179 recommends) plus 255.255.255.255.

#include "net/Sdcp.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace fruit::net {

struct DiscoveryOptions {
    std::chrono::milliseconds timeout{2500};
    std::uint16_t port = kDiscoveryPort;
    bool broadcast = true;                     // all interfaces + 255.255.255.255
    std::vector<std::string> unicastTargets;   // also ask these hosts directly
};

// Directed broadcast addresses of the up, non-loopback IPv4 interfaces.
std::vector<std::string> broadcastAddresses();

// Every printer that answered within the timeout, one entry per MainboardID.
std::vector<PrinterInfo> discover(const DiscoveryOptions& options = {});

// Asks one host directly. Throws TimeoutError if it does not answer.
PrinterInfo probe(const std::string& host, std::uint16_t port = kDiscoveryPort,
                  std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));

}  // namespace fruit::net
