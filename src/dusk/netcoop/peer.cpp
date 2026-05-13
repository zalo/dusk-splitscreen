#ifdef DUSK_NETCOOP

#include "internal.hpp"
#include "protocol.hpp"
#include "dusk/logging.h"

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

namespace dusk::netcoop::internal {

namespace {

// Read loop runs in its own thread so the WS recv() can block without
// stalling the game.
void ReadThreadMain(ws::socket_t sock) {
    auto& g = G();
    std::vector<uint8_t> frame;
    while (!g.shutdownRequested.load()) {
        if (!ws::ReadBinaryMessage(sock, &frame)) {
            DuskLog.info("netcoop: peer read closed (errno={})", ws::LastError());
            g.state.store(State::Disconnected);
            return;
        }
        if (frame.empty()) continue;

        if (frame.size() < sizeof(proto::Header)) continue;
        proto::Header h;
        std::memcpy(&h, frame.data(), sizeof(h));
        if (h.type == uint8_t(proto::MsgType::LinkState)) {
            LinkState s{};
            if (DecodeInboundFrame(frame.data(), frame.size(), &s)) {
                std::lock_guard lk(g.inMu);
                g.peerState      = s;
                g.peerStateValid = true;
            }
        } else if (h.type == uint8_t(proto::MsgType::Bye)) {
            DuskLog.info("netcoop: peer sent Bye");
            g.state.store(State::Disconnected);
            return;
        }
        // Unknown types are ignored — forward-compat by design.
    }
}

}  // namespace

void RunPeerLoop() {
    auto& g = G();
    if (g.peerSock == ws::kInvalidSocket) return;

    std::thread reader(ReadThreadMain, g.peerSock);

    using clock = std::chrono::steady_clock;
    auto last_send = clock::now();
    constexpr auto kSendInterval = std::chrono::milliseconds(16);  // ~60 Hz

    while (!g.shutdownRequested.load() &&
           g.state.load() == State::Connected) {
        std::this_thread::sleep_for(std::chrono::milliseconds(4));

        auto now = clock::now();
        if (now - last_send < kSendInterval) continue;
        last_send = now;

        LinkState snap{};
        bool have_snap = false;
        {
            std::lock_guard lk(g.outMu);
            if (g.localStateDirty) {
                snap = g.localState;
                g.localStateDirty = false;
                have_snap = true;
            }
        }
        if (!have_snap) continue;

        std::vector<uint8_t> frame;
        EncodeOutboundSnapshot(&frame, snap);
        if (!ws::SendBinaryMessage(g.peerSock, frame.data(), frame.size())) {
            DuskLog.warn("netcoop: peer write failed (errno={})", ws::LastError());
            g.state.store(State::Disconnected);
            break;
        }
    }

    ws::Close(&g.peerSock);
    if (reader.joinable()) reader.join();
}

}  // namespace dusk::netcoop::internal

#endif  // DUSK_NETCOOP
