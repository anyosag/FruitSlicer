// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "net/SdcpClient.h"

#include "net/Discovery.h"
#include "net/Md5.h"

#include <httplib.h>

#include <sys/socket.h>

#include <fstream>
#include <map>
#include <random>
#include <thread>

namespace fruit::net {

using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

std::string randomHex(std::mt19937_64& rng, std::size_t chars) {
    static const char* digits = "0123456789abcdef";
    std::string s(chars, '0');
    for (auto& c : s) c = digits[rng() & 0xF];
    return s;
}

std::int64_t unixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// A peer that drops the connection must not kill the process with SIGPIPE.
// macOS has a per-socket switch; on Linux the host program has to ignore
// SIGPIPE (fruitslicer-printer does).
void noSigpipe(socket_t sock) {
#ifdef SO_NOSIGPIPE
    int on = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#else
    (void)sock;
#endif
}

// The WebSocket went away. `delivered` says whether the request may already
// have reached the printer (lost while waiting for the answer).
struct ConnectionLost : NetError {
    ConnectionLost(const std::string& what, bool delivered) : NetError(what), delivered(delivered) {}
    bool delivered;
};

bool cancelled(const std::atomic<bool>* c) { return c && c->load(); }

// Sleeps in small steps so a cancel flag is noticed promptly.
void sleepFor(std::chrono::milliseconds d, const std::atomic<bool>* cancel) {
    const auto end = Clock::now() + d;
    while (Clock::now() < end) {
        if (cancelled(cancel)) throw CancelledError("cancelled");
        std::this_thread::sleep_for(
            std::min<Clock::duration>(std::chrono::milliseconds(50), end - Clock::now()));
    }
}

}  // namespace

std::string remotePathFor(const std::string& name) {
    if (!name.empty() && name.front() == '/') return name;
    return "/local/" + name;
}

struct SdcpClient::Impl {
    PrinterInfo printer;
    ClientOptions options;
    std::string host;
    std::mt19937_64 rng{std::random_device{}()};
    std::unique_ptr<httplib::ws::WebSocketClient> ws;

    std::map<std::string, json> responses;  // RequestID -> response Data
    json lastStatus;
    json lastAttributes;
    std::uint64_t statusSeq = 0;
    std::uint64_t attributesSeq = 0;
    std::vector<json> errors;

    void connect() {
        const std::string url =
            "ws://" + host + ":" + std::to_string(options.controlPort) + std::string(kWebSocketPath);
        ws = std::make_unique<httplib::ws::WebSocketClient>(url);
        ws->set_connection_timeout(options.connectTimeout);
        ws->set_socket_options(noSigpipe);
        auto res = ws->connect();
        if (!res)
            throw NetError("cannot open " + url + ": " + httplib::to_string(res.error()) +
                           (res.status() > 0 ? " (HTTP " + std::to_string(res.status()) + ")" : ""));
        ws->set_read_timeout(std::chrono::milliseconds(100));
        lost = false;
    }

    bool lost = false;

    // Reopens the WebSocket if it was lost. Discovery identity is kept.
    void ensureConnected() {
        if (lost || !ws || !ws->is_open()) connect();
    }

    void dispatch(const std::string& text) {
        if (text == "pong" || text == "ping") return;  // SDCP text heartbeat
        json m = json::parse(text, nullptr, false);
        if (m.is_discarded() || !m.is_object()) return;
        const std::string topic = m.value("Topic", std::string());
        auto starts = [&](const char* p) { return topic.rfind(p, 0) == 0; };
        const json* data = m.contains("Data") && m["Data"].is_object() ? &m["Data"] : nullptr;
        if (starts("sdcp/response/")) {
            if (data && data->contains("RequestID") && (*data)["RequestID"].is_string())
                responses[(*data)["RequestID"].get<std::string>()] = *data;
        } else if (starts("sdcp/status/")) {
            if (m.contains("Status")) lastStatus = m["Status"];
            else if (data && data->contains("Status")) lastStatus = (*data)["Status"];
            else return;
            ++statusSeq;
        } else if (starts("sdcp/attributes/")) {
            if (m.contains("Attributes")) lastAttributes = m["Attributes"];
            else if (data && data->contains("Attributes")) lastAttributes = (*data)["Attributes"];
            else return;
            ++attributesSeq;
        } else if (starts("sdcp/error/")) {
            errors.push_back(m);
        }
    }

    // Reads messages until `done()` or the deadline. Returns done().
    template <class Pred>
    bool pump(Clock::time_point deadline, Pred done) {
        std::string msg;
        while (!done()) {
            if (Clock::now() >= deadline) return false;
            const auto r = ws->read(msg);
            if (r == httplib::ws::Timeout) continue;
            if (r == httplib::ws::Fail) {
                lost = true;
                throw ConnectionLost("the printer closed the WebSocket connection", true);
            }
            if (r == httplib::ws::Text) dispatch(msg);
        }
        return true;
    }

    std::string send(int cmd, const json& data) {
        const std::string id = randomHex(rng, 16);
        const json req = makeRequest(printer, cmd, data, id, unixNow());
        if (!ws->send(req.dump())) {
            lost = true;
            throw ConnectionLost("WebSocket send failed", false);
        }
        return id;
    }

    // Reads whatever the printer has pushed, without waiting. Called between
    // upload chunks so its messages never pile up unread (a printer blocked
    // on a full socket could stall the upload itself). A lost connection is
    // only noted here; the next command reopens it.
    void drain() {
        if (lost || !ws || !ws->is_open()) return;
        ws->set_read_timeout(std::chrono::milliseconds(1));
        std::string msg;
        for (;;) {
            const auto r = ws->read(msg);
            if (r == httplib::ws::Timeout) break;
            if (r == httplib::ws::Fail) {
                lost = true;
                break;
            }
            if (r == httplib::ws::Text) dispatch(msg);
        }
        if (!lost) ws->set_read_timeout(std::chrono::milliseconds(100));
    }

    // One request/response, reopening a lost connection once. A request that
    // may have been delivered is sent again only if `resendable`.
    // `pushSeq`, if given, must advance (a status/attributes push) as well.
    json transact(int cmd, const json& data, std::chrono::milliseconds timeout, bool resendable,
                  const std::uint64_t* pushSeq = nullptr, const char* pushWhat = "") {
        for (int attempt = 0;; ++attempt) {
            try {
                ensureConnected();
                const std::uint64_t seq0 = pushSeq ? *pushSeq : 0;
                const auto id = send(cmd, data);
                json r = await(id, cmd, timeout);
                if (pushSeq && int(jsonInt(r, "Ack").value_or(0)) == 0 &&
                    !pump(Clock::now() + options.commandTimeout, [&] { return *pushSeq > seq0; }))
                    throw TimeoutError(std::string("printer acknowledged the request but sent no ") +
                                       pushWhat);
                return r;
            } catch (const ConnectionLost& e) {
                if (attempt > 0 || (e.delivered && !resendable)) throw;
            }
        }
    }

    json await(const std::string& id, int cmd, std::chrono::milliseconds timeout) {
        if (timeout.count() <= 0) timeout = options.commandTimeout;
        if (!pump(Clock::now() + timeout, [&] { return responses.count(id) > 0; }))
            throw TimeoutError("no response to command " + std::to_string(cmd) + " within " +
                               std::to_string(timeout.count()) + " ms");
        json r = std::move(responses[id]);
        responses.erase(id);
        return r.contains("Data") ? r["Data"] : json::object();
    }
};

SdcpClient::SdcpClient(const std::string& host, ClientOptions options)
    : impl_(std::make_unique<Impl>()) {
    impl_->options = options;
    impl_->printer = probe(host, options.udpPort, options.probeTimeout);
    // Talk to the address the user gave; MainboardIP may differ behind NAT.
    impl_->host = host;
    impl_->connect();
}

SdcpClient::SdcpClient(PrinterInfo printer, ClientOptions options)
    : impl_(std::make_unique<Impl>()) {
    impl_->options = options;
    impl_->host = printer.ip;
    impl_->printer = std::move(printer);
    impl_->connect();
}

SdcpClient::~SdcpClient() {
    if (impl_ && impl_->ws) {
        try {
            impl_->ws->close();
        } catch (...) {
        }
    }
}

const PrinterInfo& SdcpClient::printer() const { return impl_->printer; }
const std::vector<json>& SdcpClient::errors() const { return impl_->errors; }

json SdcpClient::command(Cmd cmd, const json& data, std::chrono::milliseconds timeout) {
    // Only read-only commands are sent twice after a dropped connection.
    const bool resendable = cmd == Cmd::Status || cmd == Cmd::Attributes || cmd == Cmd::FileList;
    return impl_->transact(int(cmd), data, timeout, resendable);
}

PrinterStatus SdcpClient::status() {
    // The rich status arrives as a separate push on sdcp/status/<id>.
    const json r = impl_->transact(int(Cmd::Status), json::object(), {}, true, &impl_->statusSeq,
                                   "status");
    const int ack = int(jsonInt(r, "Ack").value_or(0));
    if (ack != 0)
        throw AckError(int(Cmd::Status), ack, "status request refused (Ack " + std::to_string(ack) + ")");
    return parseStatus(impl_->lastStatus);
}

json SdcpClient::attributes() {
    const json r = impl_->transact(int(Cmd::Attributes), json::object(), {}, true,
                                   &impl_->attributesSeq, "attributes");
    const int ack = int(jsonInt(r, "Ack").value_or(0));
    if (ack != 0)
        throw AckError(int(Cmd::Attributes), ack,
                       "attributes request refused (Ack " + std::to_string(ack) + ")");
    return impl_->lastAttributes;
}

std::vector<FileEntry> SdcpClient::files(const std::string& url) {
    const json r = command(Cmd::FileList, json{{"Url", url}});
    const int ack = int(jsonInt(r, "Ack").value_or(-1));
    if (ack != 0)
        throw AckError(int(Cmd::FileList), ack,
                       "file list for " + url + " refused (Ack " + std::to_string(ack) + ")");
    return parseFileList(r);
}

void SdcpClient::cancelTransfer(const std::string& uuid, const std::string& fileName) {
    const json r = command(Cmd::CancelTransfer, json{{"Uuid", uuid}, {"FileName", fileName}});
    const int ack = int(jsonInt(r, "Ack").value_or(-1));
    if (ack != 0)
        throw AckError(int(Cmd::CancelTransfer), ack,
                       "cancel transfer refused: " + cancelTransferAckName(ack));
}

UploadResult SdcpClient::upload(const std::filesystem::path& file, const ProgressFn& progress,
                                const std::atomic<bool>* cancel, const std::string& remoteName) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec) throw NetError("cannot read " + file.string() + ": " + ec.message());
    if (size == 0) throw NetError(file.string() + " is empty");

    UploadResult res;
    res.remoteName = remoteName.empty() ? file.filename().string() : remoteName;
    if (res.remoteName.find('/') != std::string::npos)
        throw NetError("remote file name must not contain '/': " + res.remoteName);
    res.remotePath = remotePathFor(res.remoteName);
    res.size = size;
    res.md5 = Md5::hexOfFile(file);
    res.uuid = randomHex(impl_->rng, 64);

    std::ifstream in(file, std::ios::binary);
    if (!in) throw NetError("cannot open " + file.string());

    httplib::Client http(impl_->host, impl_->options.controlPort);
    http.set_socket_options(noSigpipe);
    http.set_connection_timeout(impl_->options.connectTimeout);
    http.set_read_timeout(impl_->options.chunkTimeout);
    http.set_write_timeout(impl_->options.chunkTimeout);

    const auto t0 = Clock::now();
    std::uint64_t offset = 0;
    std::string chunk;
    auto abort = [&](const std::string& why) {
        // Ask the printer to discard the partial file; report but do not mask
        // the original reason if that fails too.
        std::string extra;
        try {
            cancelTransfer(res.uuid, res.remoteName);
        } catch (const std::exception& e) {
            extra = std::string(" (and the printer did not confirm dropping the partial file: ") +
                    e.what() + ")";
        }
        throw CancelledError(why + extra);
    };

    while (offset < size) {
        if (cancelled(cancel)) abort("upload cancelled");
        const std::size_t n = std::size_t(std::min<std::uint64_t>(impl_->options.chunkSize, size - offset));
        chunk.resize(n);
        in.read(chunk.data(), std::streamsize(n));
        if (std::size_t(in.gcount()) != n) throw NetError("short read from " + file.string());

        // Field order as in saturn4-bridge, which is known to work on the device.
        httplib::UploadFormDataItems items = {
            {"Uuid", res.uuid, "", ""},
            {"Offset", std::to_string(offset), "", ""},
            {"TotalSize", std::to_string(size), "", ""},
            {"Check", "1", "", ""},
            {"S-File-MD5", res.md5, "", ""},
            {"File", chunk, res.remoteName, "application/octet-stream"},
        };
        auto r = http.Post(std::string(kUploadPath), items,
                           [&](size_t, size_t) { return !cancelled(cancel); });
        if (!r) {
            if (cancelled(cancel)) abort("upload cancelled");
            throw UploadError("upload failed at offset " + std::to_string(offset) + ": " +
                              httplib::to_string(r.error()));
        }
        if (r->status != 200)
            throw UploadError("upload refused at offset " + std::to_string(offset) + ": HTTP " +
                              std::to_string(r->status) + " " + r->body.substr(0, 200));
        const json body = json::parse(r->body, nullptr, false);
        const bool ok = body.is_object() && body.value("success", false) &&
                        body.value("code", std::string()) == "000000";
        if (!ok) {
            std::string why = r->body.substr(0, 300);
            if (body.is_object() && body.contains("messages") && body["messages"].is_array()) {
                why.clear();
                for (const auto& m : body["messages"]) {
                    if (!m.is_object()) continue;
                    if (!why.empty()) why += "; ";
                    const std::string field = m.value("field", std::string());
                    const auto code = jsonInt(m, "message");
                    if (field == "common_field" && code) why += uploadErrorName(int(*code));
                    else why += field + ": " + (m.contains("message") ? m["message"].dump() : "");
                }
            }
            throw UploadError("printer rejected the upload at offset " + std::to_string(offset) +
                              ": " + why);
        }
        offset += n;
        impl_->drain();
        if (progress) {
            const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
            progress(UploadProgress{offset, size, secs > 0 ? double(offset) / secs : 0});
        }
    }
    res.seconds = std::chrono::duration<double>(Clock::now() - t0).count();
    return res;
}

void SdcpClient::startPrint(const std::string& remotePath, const StartOptions& o,
                            const std::atomic<bool>* cancel) {
    auto log = [&](const std::string& s) {
        if (o.log) o.log(s);
    };
    // 1. Wait while the printer is still receiving/verifying a file.
    const auto settleEnd = Clock::now() + o.settleTimeout;
    for (bool first = true;; first = false) {
        const PrinterStatus st = status();
        if (st.has(MachineStatus::Printing))
            throw NetError("the printer is already printing" +
                               (st.print && !st.print->filename.empty()
                                    ? " " + st.print->filename
                                    : std::string()) +
                               "; not starting another print");
        if (!st.has(MachineStatus::FileTransferring)) break;
        if (Clock::now() >= settleEnd)
            throw TimeoutError("the printer was still verifying the file after " +
                               std::to_string(o.settleTimeout.count() / 1000) + " s");
        if (first) log("waiting for the printer to finish verifying the file");
        sleepFor(o.pollInterval, cancel);
    }
    // 2. Start, retrying on "busy" (seen on the 16K right after an upload).
    const json data{{"Filename", remotePath}, {"StartLayer", o.startLayer}};
    for (int attempt = 0;; ++attempt) {
        if (cancelled(cancel)) throw CancelledError("cancelled before starting the print");
        json r;
        try {
            r = command(Cmd::StartPrint, data, std::chrono::milliseconds(15000));
        } catch (const ConnectionLost&) {
            // The start may have reached the printer; never send it twice
            // blind. Reconnect and look.
            const PrinterStatus st = status();
            if (st.has(MachineStatus::Printing)) {
                log("connection dropped after the start command; the printer is printing");
                return;
            }
            throw NetError("the connection dropped right after the start command and the printer "
                           "is not printing; check its screen before trying again");
        }
        const int ack = int(jsonInt(r, "Ack").value_or(-1));
        if (ack == 0) return;
        if (ack == 1 && attempt < o.busyRetries) {
            log("printer busy, retrying in " + std::to_string(o.busyRetryDelay.count() / 1000) + " s");
            sleepFor(o.busyRetryDelay, cancel);
            continue;
        }
        throw AckError(int(Cmd::StartPrint), ack,
                       "printer refused to start " + remotePath + ": " + startPrintAckName(ack) +
                           (ack == 1 ? " (gave up after " + std::to_string(attempt + 1) + " tries)"
                                     : std::string()));
    }
}

}  // namespace fruit::net
