// dusk/netcoop.hpp — public API for two-instance network co-op.
//
// Two independent Dusk processes on the same machine (or LAN) discover each
// other on a port in 47100-47109, exchange a WebSocket handshake, and stream
// per-frame state. Each instance renders a "ghost-Link" actor for the remote
// player, and (in later phases) synchronizes level data, save data, and NPC AI.
//
// When DUSK_NETCOOP is undefined the entire API resolves to no-ops, so calling
// code never needs to gate on the macro itself.

#pragma once

#include <cstdint>

namespace dusk::netcoop {

#ifdef DUSK_NETCOOP

// One-time setup. Safe to call before the game has finished booting.
void Init();

// Per-frame pump. Call once after the game's main update step; drains
// inbound messages, pushes the local snapshot, and updates peer-state.
void Tick();

// Tear down sockets and worker thread. Safe to call even if Init was a no-op.
void Shutdown();

// True between successful handshake and disconnect.
bool IsActive();

// Compact wire-friendly snapshot of one Link's spatial + animation state.
// Kept POD so it can be memcpy'd straight into a WS binary frame.
struct LinkState {
    uint64_t serverFrame;  // monotonically increasing producer frame counter
    float    pos[3];       // world position (x, y, z)
    float    yaw;           // facing angle, radians
    uint16_t animIdx;       // bck index in the current animation pack
    uint16_t flags;        // bit 0: sword drawn; bit 1: shield up; ...
    float    animFrame;    // playback head, frames
};

// Push the local player's current state. Called by the game-side bridge once
// per frame just before Tick().
void SetLocalLinkState(const LinkState& s);

// Latest snapshot received from the peer (after Tick has drained the queue).
// Returns nullptr if no peer is connected or no snapshot has arrived yet.
const LinkState* GetPeerLinkState();

#else  // !DUSK_NETCOOP

struct LinkState {
    uint64_t serverFrame;
    float    pos[3];
    float    yaw;
    uint16_t animIdx;
    uint16_t flags;
    float    animFrame;
};

inline void Init() {}
inline void Tick() {}
inline void Shutdown() {}
inline bool IsActive() { return false; }
inline void SetLocalLinkState(const LinkState&) {}
inline const LinkState* GetPeerLinkState() { return nullptr; }

#endif

}  // namespace dusk::netcoop
