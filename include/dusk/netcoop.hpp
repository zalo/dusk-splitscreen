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

class fopAc_ac_c;
struct cXyz;

namespace dusk::netcoop {

// Enums are available in BOTH on/off builds so the inline broadcast hooks in
// d_com_inf_game.h can reference the IDs unconditionally. In the OFF build
// the broadcast helpers themselves are no-ops, so the IDs are inert.

enum SaveCounterId : uint16_t {
    kCounter_Rupee         = 0,
    kCounter_MaxLife       = 1,
    kCounter_KeyNum        = 2,
    kCounter_MaxMagic      = 3,
    kCounter_ArrowNum      = 4,
    kCounter_PachinkoNum   = 5,
    kCounter_MaxOil        = 6,
    kCounter_WalletSize    = 7,
    kCounter_MaxArrowNum   = 8,
    kCounter_NowOxygen     = 9,
    kCounter_MaxOxygen     = 10,
    kCounter_LASTID
};

enum SaveEquipId : uint8_t {
    kEquip_Clothes  = 0,
    kEquip_Sword    = 1,
    kEquip_Shield   = 2,
    kEquip_BButton  = 3,
    kEquip_Smell    = 4,
};

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

// --- Ghost-Link spawn machinery -----------------------------------------------
//
// daAlink_c::create() identifies the ghost by the magic value passed as the
// actor's `parameters` field at fopAcM_create time. This is a durable,
// per-actor signal — it survives the async create dispatch and doesn't get
// confused by stale slot-0 pointers left behind when a previous local Link
// is destroyed at scene transition.
constexpr uint32_t kGhostSpawnMagic = 0xC0DECAFE;

bool IsSpawningGhost();

// Register the actor pointer that came back from fopAcM_create. The netcoop
// module stores it so the rest of the engine can look up the ghost via
// dComIfGp_getPlayer(1).
void RegisterGhostActor(fopAc_ac_c* actor);

// Returns the currently-spawned ghost actor, or nullptr.
fopAc_ac_c* GetGhostActor();

// True when the actor passed in is the locally-spawned ghost.
bool IsGhost(const fopAc_ac_c* actor);

// --- AI targeting -------------------------------------------------------------
//
// Returns the closer of {local Link, ghost Link} to the actor's position.
// If no ghost is present, returns the local Link. Used by enemy AI / NPC
// queries in place of `dComIfGp_getPlayer(0)` so enemies chase whichever Link
// is closer and NPCs respond to whichever Link approached them.
fopAc_ac_c* GetNearestPlayerToActor(const fopAc_ac_c* asker);

// Position-only variant for callers that have a world point but no asking
// actor (chest open zone, carry-object grab radius, etc.).
fopAc_ac_c* GetNearestPlayer(const cXyz& from);

// --- Save-state sync ---------------------------------------------------------
//
// Hooks in the dComIfGs_* setters call these after mutating local save state.
// Each broadcast goes to the peer, which applies the same mutation via the
// same setter under a re-entry guard (IsApplyingFromPeer == true), so the
// engine's downstream callbacks fire identically on both sides.
//
// Initial state catch-up: on handshake, each peer sends a SaveSnapshot with
// the full event-bit array, item slots, counters, and equipment — so a peer
// that connects mid-game inherits whatever progress the other has already
// made.

void BroadcastEventBit(uint16_t flag, bool on);
void BroadcastCounter(uint16_t which, uint32_t value);  // see SaveCounterId enum
void BroadcastItem(uint8_t slot, uint8_t item, uint16_t count);
void BroadcastEquip(uint8_t which, uint8_t item);

// True on the netcoop reader thread while it's applying an inbound save
// mutation. Setters check this to suppress re-broadcast and avoid ping-pong.
bool IsApplyingFromPeer();

// --- Admin / status surface (used by the Settings UI) -----------------------

// Human-readable state name: "Idle" / "Searching" / "Handshaking" /
// "Connected" / "Disconnected". Cheap to call from UI render.
const char* GetStateName();

// Port we bound. 0 if we never got one.
uint16_t GetSelfPort();

// Port the peer is on. 0 if not connected.
uint16_t GetPeerPort();

// Stable 64-bit identifier for the peer process (from its Hello). 0 if unknown.
uint64_t GetPeerUuid();

// Per-second moving average of inbound + outbound message rate (post-Tick
// counters). Useful so the user can confirm traffic is flowing.
float    GetMessageRateHz();

// Drop the active connection. The worker thread re-enters discovery and tries
// to find a peer again. Safe no-op if already idle.
void     ForceDisconnect();

// Push our save state to the peer right now — re-sends the SaveSnapshot.
// Useful as a manual "resync" if anything diverged.
void     ResendSaveSnapshot();

// Teleport local Link to the peer's last-known position. No-op if no peer.
void     WarpLocalToPeer();

// Ask the peer to teleport its local Link to *our* current position.
void     WarpPeerToLocal();

// Macro form for enemy AI. Resolves to the nearest player under DUSK_NETCOOP,
// or to P1 in the OFF build (so the migration is one mechanical sed per
// enemy file and the off-build is bit-identical to upstream).
#define AI_TARGET_FOR(actor) (::dusk::netcoop::GetNearestPlayerToActor(actor))

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
inline bool IsSpawningGhost() { return false; }
inline void RegisterGhostActor(fopAc_ac_c*) {}
inline fopAc_ac_c* GetGhostActor() { return nullptr; }
inline bool IsGhost(const fopAc_ac_c*) { return false; }

// OFF build: AI_TARGET_FOR resolves directly to P1 via the global helper
// from d/d_com_inf_game.h. Callers using this macro must include that header
// so `dComIfGp_getPlayer` is visible at the call site.
#define AI_TARGET_FOR(actor) (::dComIfGp_getPlayer(0))

inline void BroadcastEventBit(uint16_t, bool) {}
inline void BroadcastCounter(uint16_t, uint32_t) {}
inline void BroadcastItem(uint8_t, uint8_t, uint16_t) {}
inline void BroadcastEquip(uint8_t, uint8_t) {}
inline bool IsApplyingFromPeer() { return false; }

inline const char* GetStateName() { return "Disabled"; }
inline uint16_t    GetSelfPort()       { return 0; }
inline uint16_t    GetPeerPort()       { return 0; }
inline uint64_t    GetPeerUuid()       { return 0; }
inline float       GetMessageRateHz()  { return 0.0f; }
inline void        ForceDisconnect()   {}
inline void        ResendSaveSnapshot() {}
inline void        WarpLocalToPeer()   {}
inline void        WarpPeerToLocal()   {}

#endif

}  // namespace dusk::netcoop
