#ifdef DUSK_NETCOOP

#include "internal.hpp"
#include "protocol.hpp"
#include "dusk/logging.h"

#include <chrono>
#include <cstring>
#include <thread>

namespace dusk::netcoop::internal {

namespace {

constexpr uint16_t kPortBase  = 47100;
constexpr uint16_t kPortCount = 10;

// Try every port in the window (except our own). On success: socket ready
// for client-side handshake, *out_port populated, returns true.
bool TryConnectAnyPeer(uint16_t self_port,
                       ws::socket_t* out_sock,
                       uint16_t* out_port) {
    for (uint16_t i = 0; i < kPortCount; ++i) {
        uint16_t p = uint16_t(kPortBase + i);
        if (p == self_port) continue;
        ws::socket_t s = ws::ConnectLoopback(p);
        if (s != ws::kInvalidSocket) {
            *out_sock = s;
            *out_port = p;
            return true;
        }
    }
    return false;
}

bool SendHello(ws::socket_t s) {
    auto& g = G();
    proto::Header h{};
    h.type    = uint8_t(proto::MsgType::Hello);
    h.bodyLen = sizeof(proto::Hello);

    proto::Hello hello{};
    hello.version      = proto::kVersion;
    hello.instanceUuid = g.instanceUuid;
    hello.selfPort     = g.selfPort;
    hello.saveSlot     = 0;
    std::strncpy(hello.buildId, "dev", sizeof(hello.buildId) - 1);

    uint8_t buf[sizeof(h) + sizeof(hello)];
    std::memcpy(buf,               &h,     sizeof(h));
    std::memcpy(buf + sizeof(h),    &hello, sizeof(hello));
    return ws::SendBinaryMessage(s, buf, sizeof(buf));
}

bool RecvHello(ws::socket_t s, proto::Hello* out) {
    std::vector<uint8_t> frame;
    if (!ws::ReadBinaryMessage(s, &frame)) return false;
    if (frame.size() != sizeof(proto::Header) + sizeof(proto::Hello)) return false;
    proto::Header h;
    std::memcpy(&h, frame.data(), sizeof(h));
    if (h.type != uint8_t(proto::MsgType::Hello)) return false;
    if (h.bodyLen != sizeof(proto::Hello)) return false;
    std::memcpy(out, frame.data() + sizeof(h), sizeof(*out));
    return out->version == proto::kVersion;
}

}  // namespace

bool RunDiscovery() {
    auto& g = G();

    // 1. Bind on the first free port in the window.
    g.listenSock = ws::ListenLoopback(kPortBase, kPortCount, &g.selfPort);
    if (g.listenSock == ws::kInvalidSocket) {
        DuskLog.warn("netcoop: no free port in {}-{} (errno={})",
                     kPortBase, kPortBase + kPortCount - 1, ws::LastError());
        return false;
    }
    DuskLog.info("netcoop: listening on 127.0.0.1:{} (uuid={:016x})",
                 g.selfPort, g.instanceUuid);

    // 2. Optimistically probe peer ports — if another instance is already up,
    //    we'll connect immediately. Otherwise fall through to accept().
    g.state.store(State::Searching);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline) {
        if (g.shutdownRequested.load()) return false;

        ws::socket_t client_sock = ws::kInvalidSocket;
        uint16_t peer_port = 0;
        if (TryConnectAnyPeer(g.selfPort, &client_sock, &peer_port)) {
            DuskLog.info("netcoop: dialed peer on 127.0.0.1:{}", peer_port);
            char host[32];
            std::snprintf(host, sizeof(host), "127.0.0.1:%u", peer_port);
            if (!ws::ClientHandshake(client_sock, host)) {
                ws::Close(&client_sock);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            g.peerSock    = client_sock;
            g.peerPort    = peer_port;
            g.clientRole  = true;
            break;
        }

        // No peer yet — poll for an incoming connection between sleeps.
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

        ws::socket_t incoming = ws::AcceptNonBlocking(g.listenSock);
        if (incoming != ws::kInvalidSocket) {
            if (!ws::ServerHandshake(incoming)) {
                ws::Close(&incoming);
                continue;
            }
            g.peerSock   = incoming;
            g.clientRole = false;
            break;
        }
    }

    if (g.peerSock == ws::kInvalidSocket) {
        DuskLog.info("netcoop: no peer found within 60s; staying single-player");
        return false;
    }

    // 3. Exchange Hello in both directions.
    g.state.store(State::Handshaking);
    if (!SendHello(g.peerSock)) {
        DuskLog.warn("netcoop: failed to send Hello");
        ws::Close(&g.peerSock);
        return false;
    }
    proto::Hello remote{};
    if (!RecvHello(g.peerSock, &remote)) {
        DuskLog.warn("netcoop: failed to receive Hello");
        ws::Close(&g.peerSock);
        return false;
    }

    DuskLog.info("netcoop: peer handshake OK — uuid={:016x} save={} role={}",
                 remote.instanceUuid, remote.saveSlot,
                 g.clientRole ? "client" : "server");

    g.state.store(State::Connected);
    return true;
}

}  // namespace dusk::netcoop::internal

#endif  // DUSK_NETCOOP
