// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "net/Discovery.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <set>

namespace fruit::net {
namespace {

class UdpSocket {
public:
    UdpSocket() {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd_ < 0) throw NetError(std::string("socket: ") + std::strerror(errno));
        sockaddr_in any{};
        any.sin_family = AF_INET;
        any.sin_addr.s_addr = htonl(INADDR_ANY);
        any.sin_port = 0;
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&any), sizeof any) < 0) {
            const int e = errno;
            ::close(fd_);
            throw NetError(std::string("bind: ") + std::strerror(e));
        }
    }
    ~UdpSocket() { ::close(fd_); }
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    void enableBroadcast() {
        int on = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
    }

    // Returns false if the address could not be resolved or the send failed.
    bool sendTo(const std::string& host, std::uint16_t port, std::string_view msg) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
        sockaddr_in to = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
        ::freeaddrinfo(res);
        to.sin_port = htons(port);
        return ::sendto(fd_, msg.data(), msg.size(), 0, reinterpret_cast<sockaddr*>(&to),
                        sizeof to) == ssize_t(msg.size());
    }

    // Waits up to `ms` for one datagram.
    bool receive(int ms, std::string& data, std::string& from) {
        pollfd p{fd_, POLLIN, 0};
        const int r = ::poll(&p, 1, std::max(ms, 0));
        if (r <= 0) return false;
        char buf[16384];
        sockaddr_in src{};
        socklen_t len = sizeof src;
        const ssize_t n =
            ::recvfrom(fd_, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&src), &len);
        if (n < 0) return false;
        data.assign(buf, std::size_t(n));
        char ip[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &src.sin_addr, ip, sizeof ip);
        from = ip;
        return true;
    }

private:
    int fd_ = -1;
};

}  // namespace

std::vector<std::string> broadcastAddresses() {
    std::vector<std::string> out;
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) return out;
    for (ifaddrs* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
        if (!(i->ifa_flags & IFF_BROADCAST)) continue;
        // Compute from address and netmask rather than trusting ifa_broadaddr,
        // whose name differs between Linux and BSD.
        if (!i->ifa_netmask) continue;
        const auto addr = reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr;
        const auto mask = reinterpret_cast<sockaddr_in*>(i->ifa_netmask)->sin_addr.s_addr;
        in_addr b{};
        b.s_addr = addr | ~mask;
        char ip[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &b, ip, sizeof ip);
        if (std::find(out.begin(), out.end(), ip) == out.end()) out.emplace_back(ip);
    }
    ::freeifaddrs(list);
    return out;
}

std::vector<PrinterInfo> discover(const DiscoveryOptions& options) {
    UdpSocket sock;
    std::vector<std::string> targets;
    if (options.broadcast) {
        sock.enableBroadcast();
        targets = broadcastAddresses();
        targets.emplace_back("255.255.255.255");
    }
    for (const auto& t : options.unicastTargets) targets.push_back(t);
    bool sentAny = false;
    for (const auto& t : targets) sentAny |= sock.sendTo(t, options.port, kDiscoveryMessage);
    if (!sentAny && !targets.empty())
        throw NetError("discovery: could not send to any address (no network?)");

    std::vector<PrinterInfo> found;
    std::set<std::string> seen;
    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) break;
        std::string data, from;
        if (!sock.receive(int(left), data, from)) continue;
        if (data == kDiscoveryMessage) continue;  // our own broadcast looped back
        auto p = parseDiscoveryReply(data, from);
        if (p && seen.insert(p->mainboardId).second) found.push_back(std::move(*p));
    }
    return found;
}

PrinterInfo probe(const std::string& host, std::uint16_t port,
                  std::chrono::milliseconds timeout) {
    // Unlike discover(), return on the first answer instead of waiting out the timeout.
    UdpSocket sock;
    if (!sock.sendTo(host, port, kDiscoveryMessage))
        throw NetError("cannot send to " + host + " (bad address?)");
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) break;
        std::string data, from;
        if (!sock.receive(int(left), data, from)) continue;
        if (auto p = parseDiscoveryReply(data, from)) return *p;
    }
    throw TimeoutError("no SDCP answer from " + host + " on UDP port " + std::to_string(port) +
                       " (wrong address, printer off, or not on this network?)");
}

}  // namespace fruit::net
