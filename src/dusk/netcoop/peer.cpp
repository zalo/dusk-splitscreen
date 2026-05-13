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

        g.msgsIn.fetch_add(1);

        switch (proto::MsgType(h.type)) {
            case proto::MsgType::LinkState: {
                LinkState s{};
                if (DecodeInboundFrame(frame.data(), frame.size(), &s)) {
                    std::lock_guard lk(g.inMu);
                    g.peerState      = s;
                    g.peerStateValid = true;
                }
                break;
            }
            case proto::MsgType::SaveBit:
            case proto::MsgType::SaveCounter:
            case proto::MsgType::SaveItem:
            case proto::MsgType::SaveEquip:
            case proto::MsgType::SaveSnapshot:
            case proto::MsgType::WarpRequest: {
                // Hand off to the game thread; it applies under the re-entry
                // guard so the in-engine setter side effects fire normally.
                std::lock_guard lk(g.saveInMu);
                g.saveInQueue.push_back(frame);
                if (h.type == uint8_t(proto::MsgType::SaveSnapshot)) {
                    g.snapshotReceived = true;
                }
                break;
            }
            case proto::MsgType::Bye:
                DuskLog.info("netcoop: peer sent Bye");
                g.state.store(State::Disconnected);
                return;
            default:
                // Forward-compat: ignore unknown types.
                break;
        }
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

    // The initial SaveSnapshot is captured + queued by Tick (game thread) so
    // we don't race on reading global save state from a non-game thread.

    while (!g.shutdownRequested.load() &&
           g.state.load() == State::Connected) {
        if (g.forceDisconnectRequested.load()) {
            DuskLog.info("netcoop: admin requested disconnect — dropping peer");
            g.state.store(State::Disconnected);
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(4));

        // Drain save-state queue — these are reliable, send each one.
        std::vector<std::vector<uint8_t>> outSaves;
        {
            std::lock_guard lk(g.saveOutMu);
            outSaves.swap(g.saveOutQueue);
        }
        for (const auto& buf : outSaves) {
            if (!ws::SendBinaryMessage(g.peerSock, buf.data(), buf.size())) {
                DuskLog.warn("netcoop: save write failed (errno={})", ws::LastError());
                g.state.store(State::Disconnected);
                break;
            }
            g.msgsOut.fetch_add(1);
        }
        if (g.state.load() != State::Connected) break;

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
        g.msgsOut.fetch_add(1);
    }

    ws::Close(&g.peerSock);
    if (reader.joinable()) reader.join();
}

}  // namespace dusk::netcoop::internal

#endif  // DUSK_NETCOOP
