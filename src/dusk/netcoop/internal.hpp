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

class fopAc_ac_c;

namespace dusk::netcoop::proto {
struct SaveSnapshotMsg;
}

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
    uint64_t peerUuid     = 0;     // set after Hello exchange
    uint16_t selfPort = 0;
    uint16_t peerPort = 0;
    bool     clientRole = false;  // higher-port instance dials lower-port

    // Traffic counters (sampled by the UI once per second to render rate).
    std::atomic<uint64_t> msgsIn{0};
    std::atomic<uint64_t> msgsOut{0};

    // Admin: when ForceDisconnect is requested we close the peer socket so
    // the worker drops back to discovery. Drained by the worker thread.
    std::atomic<bool> forceDisconnectRequested{false};
    std::atomic<bool> resendSnapshotRequested{false};

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

    // Ghost-Link bookkeeping (touched only from the game thread).
    fopAc_ac_c*  ghostActor      = nullptr;
    bool         spawningGhost   = false;  // true around fopAcM_create call
    bool         ghostSpawnPending = false;  // create() requested, daAlink not yet registered

    // Save-state replication: outbound event queue. Each entry is an already-
    // serialized binary WS payload (Header + Msg body) — the sender just hands
    // them straight to ws::SendBinaryMessage. Pushed from the game thread,
    // drained by the sender thread.
    std::mutex                       saveOutMu;
    std::vector<std::vector<uint8_t>> saveOutQueue;

    // Inbound save mutations: pushed by the reader thread, drained by the
    // game thread (Tick). Keeping the apply on the game thread preserves the
    // engine's single-writer assumption for save state.
    std::mutex                       saveInMu;
    std::vector<std::vector<uint8_t>> saveInQueue;

    // Set true on the reader thread while applying an inbound save mutation;
    // setters check via IsApplyingFromPeer to suppress re-broadcast.
    // Threadlocal lives in the .cpp.

    // True once both peers have exchanged the post-handshake SaveSnapshot.
    bool snapshotSent     = false;
    bool snapshotReceived = false;

    // Host sends one WorldLocation right after handshake so the client can
    // load into the same stage/room.
    bool worldLocationSent = false;
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

// Implemented in netcoop.cpp. Called from the game thread.
void CaptureSaveSnapshot(proto::SaveSnapshotMsg* out);
void ApplySaveSnapshot(const proto::SaveSnapshotMsg& snap);
void DrainSaveInbound();

}  // namespace dusk::netcoop::internal

#endif  // DUSK_NETCOOP
