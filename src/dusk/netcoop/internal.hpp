// Module-internal globals shared across netcoop TUs.
// Game code should #include "dusk/netcoop.hpp", not this header.

#pragma once

#ifdef DUSK_NETCOOP

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "dusk/netcoop.hpp"
#include "ws.hpp"

namespace dusk::netcoop::internal {

enum class State : int {
    Idle,             // before Init or after Shutdown
    Searching,        // listening + probing peer ports
    Handshaking,      // socket up, exchanging Hello
    Connected,        // peer fully linked
    Disconnected,     // peer dropped; will retry
};

struct Globals {
    std::atomic<State> state{State::Idle};
    std::atomic<bool>  shutdownRequested{false};

    // Discovery / identity.
    uint64_t instanceUuid = 0;
    uint16_t selfPort = 0;
    uint16_t peerPort = 0;
    bool     clientRole = false;  // higher-port instance dials lower-port

    // Active sockets.
    ws::socket_t listenSock = ws::kInvalidSocket;
    ws::socket_t peerSock   = ws::kInvalidSocket;

    // Outbound (set by game thread, read by net thread).
    std::mutex   outMu;
    LinkState    localState{};
    bool         localStateDirty = false;

    // Inbound (set by net thread, read by game thread).
    std::mutex   inMu;
    LinkState    peerState{};
    bool         peerStateValid = false;
};

Globals& G();

// Implemented in discovery.cpp.
bool RunDiscovery();

// Implemented in peer.cpp. Blocks running the read loop until the peer
// disconnects or shutdown is requested.
void RunPeerLoop();

// Implemented in sync.cpp.
void EncodeOutboundSnapshot(std::vector<uint8_t>* frame, const LinkState& s);
bool DecodeInboundFrame(const uint8_t* data, size_t len, LinkState* out);

}  // namespace dusk::netcoop::internal

#endif  // DUSK_NETCOOP
