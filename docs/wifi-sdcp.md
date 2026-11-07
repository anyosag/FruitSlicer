# Wi-Fi sending: SDCP V3 notes

What `src/net` (library `fruit_net`, CLI `fruitslicer-printer`) relies on,
where each detail comes from, and what is still unverified. Sources were read
on 2026-11-07. Nothing below was tested against a real printer by FruitSlicer
yet; "verified" means "stated in the source cited", and the device column says
whether someone reports it from a real Saturn 4 Ultra (16K).

Sources:

- **[Spec]** CBD/Chitu, *SDCP V3.0.0*, English text —
  <https://github.com/cbd-tech/SDCP-Smart-Device-Control-Protocol-V3.0.0>
  (no licence file: FruitSlicer implements from it and does not copy it).
- **[16K]** Alfie Dennen, real-device notes, Saturn 4 Ultra 16K, firmware V1.5.6 (MIT) —
  <https://github.com/alfiedennen/sdcp-saturn-4-ultra> (README and `camera-stream-leak.md`).
- **[S4B]** Bulrok, `saturn4-bridge-linux`, a working CLI for the Saturn 4 Ultra (GPL-3.0; read, not copied) —
  <https://github.com/Bulrok/saturn4-bridge-linux> (`saturn4.js`, README).
- **[HA]** Daniel Cherubini, `elegoo-homeassistant` (MIT), commit 7214c37 —
  <https://github.com/danielcherubini/elegoo-homeassistant>, and its
  [issue #451](https://github.com/danielcherubini/elegoo-homeassistant/issues/451).
- **[TN3179]** Apple, *TN3179: Understanding local network privacy* —
  <https://developer.apple.com/documentation/technotes/tn3179-understanding-local-network-privacy>.

## Protocol details used

| Detail | Source | On a real 16K? |
|---|---|---|
| Discovery: ASCII `M99999` to UDP port 3000; JSON reply `{Id, Data{Name, MachineName, BrandName, MainboardIP, MainboardID, ProtocolVersion, FirmwareVersion}}` | Spec | Yes [16K], [S4B] (both use it). [16K] says the 16K's reply carries `Data.Attributes` and `Data.Status`; [HA] parses both `Data.Attributes.*` and flat `Data.*`, and so does FruitSlicer. |
| Unicast `M99999` to one IP also answers | [S4B] (`connect()` probes the given IP) | Yes, per [S4B] |
| Every request needs `Id` and `MainboardID` from discovery | [S4B] README | Yes |
| Control: WebSocket `ws://<ip>:3030/websocket` | Spec | Yes [16K], [S4B] |
| Topics `sdcp/{request,response,status,attributes,error,notice}/<MainboardID>` | Spec | Yes [16K] |
| Request envelope `{Id, Data{Cmd, Data, RequestID, MainboardID, TimeStamp, From}, Topic}`; `From: 0` (LAN PC) | Spec; [16K] repeats it | Yes |
| `RequestID`: spec example is 16 hex chars; [HA] sends 16 hex, [S4B] 32 hex. FruitSlicer sends 16. | Spec, [HA], [S4B] | Probably both: [S4B] runs on a Saturn 4 Ultra, and [HA] (16 hex) has 16K users (issue #451 reports live 16K sensor data) |
| Responses on `sdcp/response/…` matched by `RequestID`; rich status/attributes come as separate pushes (top-level `Status` / `Attributes` keys); Cmd 1's reply carries no attributes | [16K], [S4B] | Yes |
| `CurrentStatus` is an array in the spec and over WebSocket; [HA] notes MQTT printers send an int. Both accepted. | Spec, [HA] | Array: yes |
| Status codes 0 idle, 1 printing, 2 file transferring, 3 exposure test, 4 self-check | Spec | — |
| Text heartbeat `"ping"` → `"pong"` | Spec | Not exercised. FruitSlicer relies on WebSocket protocol pings instead (cpp-httplib sends one every 30 s, as [HA]'s aiohttp `heartbeat=30` does), and ignores `pong` texts. |
| Cmd 0 status, 1 attributes, 128 start `{Filename, StartLayer}`, 129 pause, 130 stop, 131 resume, 255 cancel transfer `{Uuid, FileName}`, 258 file list `{Url}`, 259 delete `{FileList, FolderList}` | Spec | 0, 1, 128–131, 258, 259 yes [16K]. **255 not reported on a device by anyone.** |
| Cmd 128 Ack: 0 OK, 1 busy, 2 not found, 3 MD5 failed, 4 read failed, 5 resolution mismatch, 6 unknown format, 7 model mismatch | Spec | Ack 0 and 1 seen [16K]. (Note [S4B]'s error table uses a different, unsourced numbering; FruitSlicer follows the spec.) |
| Cmd 255 Ack: 0 OK, 1 not transferring, 2 already verifying, 3 not found | Spec | No |
| `Filename` for Cmd 128 is a path such as `/local/x.goo` | [S4B] (spec example is a bare name) | Yes, per [S4B] |
| Cmd 258 `FileList[].type`: 1 file, 0 folder; `Url` `/local` or `/usb` | Spec, [16K] | Yes |
| Cmd 259 answers Ack 0 even for a path that does not exist | [16K] | Yes |
| Cmds 132, 133, 260 never answer on resin firmware | [16K] | Yes. Not used; commands time out instead of hanging. |
| Upload: multipart `POST http://<ip>:3030/uploadFile/upload`, 1 MB chunks, fields `S-File-MD5` (whole-file MD5), `Check` (`1`), `Offset`, `Uuid` (same for every chunk), `TotalSize`, `File` | Spec | Yes [16K], [S4B] (1 MiB = 1 048 576-byte chunks). Each chunk is sent with a `Content-Length` (not chunked transfer encoding) and without `Expect: 100-continue`, with the fields in [S4B]'s order (Uuid, Offset, TotalSize, Check, S-File-MD5, File), as `fetch(FormData)` in [S4B] does. |
| Upload `Uuid`: spec shows 32 hex chars (Cmd 255 example); [S4B] sends 64 hex chars. FruitSlicer sends 64. | Spec, [S4B] | 64: yes, per [S4B] |
| Upload success `{"code":"000000","success":true}`; failure `code "111111"` with `messages[{field:"common_field", message:<code>}]`, codes −1 offset < 0, −2 offset mismatch, −3 open failed, −4 unknown | Spec | Success shape: [S4B] checks `success`. Failure shapes: not seen. |
| ~170 KB/s upload over the 16K's Wi-Fi (66 MB ≈ 7 min) | [16K] | Yes |
| After an upload the printer verifies the file (status 2) and Cmd 128 answers Ack 1 (busy) even when the file is already listed; ~15 s, or wait until status leaves 2, then retry | [16K], [S4B] (`send --start` polls status 2) | Yes |
| No authentication | [S4B] README; nothing in the spec | Yes |

### What happens on an MD5 mismatch — unverified

The spec says the printer pushes `sdcp/error/<id>` with `ErrorCode 1` ("file
transfer MD5 check failed"), but not what the last chunk's HTTP reply is.
FruitSlicer treats any non-success chunk reply as a failure, records error
pushes (`SdcpClient::errors()`), and Cmd 128 would answer Ack 3. The mock
printer rejects the last chunk; real firmware may instead answer success and
only push the error.

### Camera: never touched

[16K] (`camera-stream-leak.md`) and [HA] issue #451 report that the 16K keeps
RTSP video sessions in its table when a client dies without `TEARDOWN`; the
counter (`NumberOfVideoStreamConnected`, cap 2) never recovers, Cmd 386 cannot
clear it, and only a power-cycle does. `fruit_net` has no camera, video-stream
(386) or time-lapse (387) code, and must not gain any without a design that
deals with this.

## macOS local network privacy

From [TN3179], "macOS considerations": macOS automatically allows local
network access for launchd daemons, programs running as root, and
"command-line tools run from Terminal or over SSH, including any child
processes they spawn". So `fruitslicer-printer` run from Terminal needs no
permission. An app (the planned "Send over Wi-Fi" button) is not exempt: it
needs `NSLocalNetworkUsageDescription` in its `Info.plist`, and the user sees
the local network alert on first use. Broadcast (255.255.255.255) and
multicast addresses count as local network addresses; the
`com.apple.developer.networking.multicast` entitlement is described as an iOS
requirement. TN3179 also recommends, for UDP-broadcast discovery that cannot
use Bonjour, running discovery "on all broadcast-capable interfaces" rather
than assuming `en0` — which `discover()` does (directed broadcast on every up,
non-loopback IPv4 interface with `IFF_BROADCAST`, plus 255.255.255.255). It
notes bugs fixed in macOS 15.1.

## Dependency

cpp-httplib **v0.62.0** (tag commit `74ef3c4`, released 2026-10-18), MIT
licence, header-only, pinned in `cmake/Dependencies.cmake`. It provides the
WebSocket client (and server, used by the mock), and multipart POST. Built
without TLS, zlib, brotli or zstd: SDCP is plain HTTP/WS on the LAN.

## Open questions only the real printer can answer

1. Whether Cmd 255 (cancel transfer) works on the 16K, and what it answers.
2. What the HTTP reply to the last chunk is when the MD5 does not match.
3. How long the busy window after an upload really is for a large file
   (FruitSlicer waits up to 3 min for status to leave "transferring", then
   retries busy 6 × 5 s).
4. Whether the 16K's discovery reply is flat or nested (both parsed).
