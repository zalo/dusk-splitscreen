#ifdef DUSK_NETCOOP

#include "dusk/netcoop.hpp"
#include "internal.hpp"
#include "protocol.hpp"
#include "dusk/logging.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "f_op/f_op_actor.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include "d/d_com_inf_game.h"
#include "d/d_save.h"
#include "SSystem/SComponent/c_xyz.h"
#include "SSystem/SComponent/c_sxyz.h"
#include "dusk/ui/ui.hpp"

namespace dusk::netcoop {
namespace internal {

Globals& G() {
    static Globals g;
    return g;
}

// Worker thread lives at internal-namespace scope so Init/Shutdown can manage it.
std::thread g_worker;

namespace {

void WorkerMain() {
    auto& g = G();
    while (!g.shutdownRequested.load()) {
        if (RunDiscovery()) {
            RunPeerLoop();
        }
        if (g.shutdownRequested.load()) break;

        ws::Close(&g.listenSock);
        ws::Close(&g.peerSock);
        {
            std::lock_guard lk(g.inMu);
            g.peerStateValid = false;
        }
        // Reset connection-scoped state so the next handshake starts fresh.
        g.snapshotSent     = false;
        g.snapshotReceived = false;
        g.worldLocationSent = false;
        g.peerUuid         = 0;
        g.peerPort         = 0;
        g.forceDisconnectRequested.store(false);
        g.resendSnapshotRequested.store(false);
        g.state.store(State::Idle);

        // Backoff before another full discovery sweep.
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    ws::Close(&g.listenSock);
    ws::Close(&g.peerSock);
}

}  // namespace
}  // namespace internal

// Forward-declared so Tick() and the Broadcast* helpers can both push frames
// onto the outbound queue; defined later in this TU.
namespace {
template <typename Body>
void PushSaveOut(proto::MsgType type, const Body& body);
}

void Init() {
    auto& g = internal::G();
    if (internal::g_worker.joinable()) return;  // already running

    ws::GlobalInit();

    std::random_device rd;
    g.instanceUuid = (uint64_t(rd()) << 32) | uint64_t(rd());

    g.shutdownRequested.store(false);
    internal::g_worker = std::thread(internal::WorkerMain);
    DuskLog.info("netcoop: Init (uuid={:016x})", g.instanceUuid);
}

namespace internal {

// Spawn the ghost daAlink next to the local Link once both sides are ready.
// Called from Tick on the game thread.
static void MaybeSpawnGhost() {
    auto& g = G();
    if (g.ghostActor != nullptr) return;
    if (g.ghostSpawnPending) return;  // create() in flight, wait for register
    if (g.state.load() != State::Connected) return;

    fopAc_ac_c* local = dComIfGp_getPlayer(0);
    if (local == nullptr) return;  // wait for P1

    cXyz spawn_pos = local->current.pos;
    spawn_pos.x += 100.0f;  // 1 metre lateral so we can see them
    csXyz angle{0, local->shape_angle.y, 0};

    g.spawningGhost   = true;
    g.ghostSpawnPending = true;
    // Pass kGhostSpawnMagic as the actor's parameters so daAlink_c::create
    // can unambiguously identify this spawn as the ghost, independent of
    // when create() actually runs or what's in slot 0.
    fpc_ProcID pid = fopAcM_create(
        fpcNm_ALINK_e,
        /*parameters=*/kGhostSpawnMagic,
        &spawn_pos,
        fopAcM_GetRoomNo(local),
        &angle,
        /*scale=*/nullptr,
        /*argument=*/-1);
    g.spawningGhost = false;

    if (pid == 0) {
        g.ghostSpawnPending = false;
        DuskLog.warn("netcoop: ghost fopAcM_create returned 0; will retry");
        return;
    }
    DuskLog.info("netcoop: requested ghost spawn proc_id={}", pid);
}

static void MaybeDespawnGhost() {
    auto& g = G();
    if (g.ghostActor == nullptr) return;
    if (g.state.load() == State::Connected) return;  // still up

    // Use fpc machinery to remove the actor cleanly.
    base_process_class* proc = reinterpret_cast<base_process_class*>(g.ghostActor);
    fpcM_Delete(proc);
    g.ghostActor = nullptr;
    g_dComIfG_gameInfo.play.setPlayer(1, nullptr);
    DuskLog.info("netcoop: ghost despawned");
}

}  // namespace internal

void Tick() {
    auto& g = internal::G();

    // Detect transitions in/out of Connected so we can pop a "Player joined"
    // or "Player left" toast — mirrors how the controller-connected toast
    // works (push_toast from the game thread).
    static bool s_wasConnected = false;
    const bool nowConnected = (g.state.load() == internal::State::Connected);
    if (nowConnected && !s_wasConnected) {
        ::dusk::ui::push_toast({
            .type     = "netcoop",
            .title    = "Player joined",
            .content  = fmt::format("Co-op peer on 127.0.0.1:{}", g.peerPort),
            .duration = std::chrono::seconds(4),
        });
        DuskLog.info("netcoop: toast — Player joined (port {})", g.peerPort);
    } else if (!nowConnected && s_wasConnected) {
        ::dusk::ui::push_toast({
            .type     = "netcoop",
            .title    = "Player left",
            .content  = "Co-op peer disconnected",
            .duration = std::chrono::seconds(4),
        });
        DuskLog.info("netcoop: toast — Player left");
    }
    s_wasConnected = nowConnected;

    internal::MaybeDespawnGhost();
    internal::MaybeSpawnGhost();
    internal::DrainSaveInbound();

    if (g.state.load() != internal::State::Connected) return;

    // Manual "Resend snapshot" admin button: rearm the snapshot flag so the
    // block below queues a fresh capture.
    if (g.resendSnapshotRequested.exchange(false)) {
        g.snapshotSent = false;
    }

    // First Tick after the handshake: capture our save state and queue it as
    // a SaveSnapshot so the peer can merge our progress. Captured on the game
    // thread to avoid racing with engine writes.
    if (!g.snapshotSent) {
        proto::SaveSnapshotMsg snap{};
        internal::CaptureSaveSnapshot(&snap);

        proto::Header h{};
        h.type    = uint8_t(proto::MsgType::SaveSnapshot);
        h.bodyLen = sizeof(snap);

        std::vector<uint8_t> buf(sizeof(h) + sizeof(snap));
        std::memcpy(buf.data(),             &h,    sizeof(h));
        std::memcpy(buf.data() + sizeof(h), &snap, sizeof(snap));
        {
            std::lock_guard lk(g.saveOutMu);
            g.saveOutQueue.push_back(std::move(buf));
        }
        g.snapshotSent = true;
        DuskLog.info("netcoop: queued initial SaveSnapshot");
    }

    // Host (server role, lower-port) broadcasts its current stage/room/pos
    // exactly once after handshake so the client can warp/load to match.
    if (!g.worldLocationSent && !g.clientRole) {
        const char* stageName = dComIfGp_getStartStageName();
        fopAc_ac_c* local = dComIfGp_getPlayer(0);
        if (stageName != nullptr && stageName[0] != 0 && local != nullptr) {
            proto::WorldLocationMsg body{};
            std::strncpy(body.stage, stageName, sizeof(body.stage));
            body.point  = dComIfGp_getStartStagePoint();
            body.roomNo = dComIfGp_getStartStageRoomNo();
            body.layer  = dComIfGp_getStartStageLayer();
            body.pos[0] = local->current.pos.x;
            body.pos[1] = local->current.pos.y;
            body.pos[2] = local->current.pos.z;
            body.yaw    = static_cast<float>(local->shape_angle.y) *
                          (3.14159265f / 32768.0f);
            PushSaveOut(proto::MsgType::WorldLocation, body);
            g.worldLocationSent = true;
            DuskLog.info("netcoop: host broadcast WorldLocation stage={} room={} pt={}",
                         body.stage, body.roomNo, body.point);
        }
    }

    // Once per ~6 seconds, sample-log the peer's position so two-instance runs
    // can be eyeballed in the log without spamming.
    static uint64_t s_tickCounter = 0;
    if ((++s_tickCounter % 360) == 0) {
        const LinkState* peer = GetPeerLinkState();
        if (peer) {
            DuskLog.info("netcoop: peer @ ({:.1f}, {:.1f}, {:.1f}) yaw={:.2f} anm={} frame={:.1f}",
                         peer->pos[0], peer->pos[1], peer->pos[2],
                         peer->yaw, peer->animIdx, peer->animFrame);
        }
    }
}

bool IsSpawningGhost() {
    return internal::G().spawningGhost;
}

void RegisterGhostActor(fopAc_ac_c* actor) {
    auto& g = internal::G();
    g.ghostActor        = actor;
    g.ghostSpawnPending = false;
    g_dComIfG_gameInfo.play.setPlayerInfo(1, actor, /*cameraID=*/0);
    DuskLog.info("netcoop: ghost actor registered ({})", static_cast<void*>(actor));
}

fopAc_ac_c* GetGhostActor() {
    return internal::G().ghostActor;
}

bool IsGhost(const fopAc_ac_c* actor) {
    return actor != nullptr && actor == internal::G().ghostActor;
}

namespace {

inline float DistSq(const cXyz& a, const cXyz& b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

}  // namespace

fopAc_ac_c* GetNearestPlayer(const cXyz& from) {
    fopAc_ac_c* local = dComIfGp_getPlayer(0);
    fopAc_ac_c* ghost = internal::G().ghostActor;
    if (ghost == nullptr) return local;
    if (local == nullptr) return ghost;
    return DistSq(ghost->current.pos, from) < DistSq(local->current.pos, from)
           ? ghost : local;
}

fopAc_ac_c* GetNearestPlayerToActor(const fopAc_ac_c* asker) {
    fopAc_ac_c* local = dComIfGp_getPlayer(0);
    fopAc_ac_c* ghost = internal::G().ghostActor;
    if (asker == nullptr) return local;
    if (ghost == nullptr || ghost == asker) return local;
    if (local == nullptr || local == asker) return ghost;
    return GetNearestPlayer(asker->current.pos);
}

// ---------------------------------------------------------------------------
// Save-state replication
// ---------------------------------------------------------------------------

namespace {

// True while applying an inbound save mutation; setters check this to
// suppress re-broadcast so the bit doesn't ping-pong between peers.
thread_local bool t_applyingFromPeer = false;

template <typename Body>
void PushSaveOut(proto::MsgType type, const Body& body) {
    auto& g = internal::G();
    if (g.state.load() != internal::State::Connected) return;

    proto::Header h{};
    h.type    = uint8_t(type);
    h.bodyLen = sizeof(body);

    std::vector<uint8_t> buf(sizeof(h) + sizeof(body));
    std::memcpy(buf.data(),                &h,    sizeof(h));
    std::memcpy(buf.data() + sizeof(h),    &body, sizeof(body));

    std::lock_guard lk(g.saveOutMu);
    g.saveOutQueue.push_back(std::move(buf));
}

uint32_t ReadCounter(uint16_t which) {
    switch (which) {
        case kCounter_Rupee:       return uint32_t(dComIfGs_getRupee());
        case kCounter_MaxLife:     return uint32_t(dComIfGs_getMaxLife());
        case kCounter_KeyNum:      return uint32_t(dComIfGs_getKeyNum());
        case kCounter_MaxMagic:    return uint32_t(dComIfGs_getMaxMagic());
        case kCounter_ArrowNum:    return uint32_t(dComIfGs_getArrowNum());
        case kCounter_PachinkoNum: return uint32_t(dComIfGs_getPachinkoNum());
        case kCounter_MaxOil:      return uint32_t(dComIfGs_getMaxOil());
        case kCounter_WalletSize:  return uint32_t(dComIfGs_getWalletSize());
        default: return 0;
    }
}

void WriteCounter(uint16_t which, uint32_t value) {
    switch (which) {
        case kCounter_Rupee:       dComIfGs_setRupee(u16(value)); break;
        case kCounter_MaxLife:     dComIfGs_setMaxLife(u8(value)); break;
        case kCounter_KeyNum:      dComIfGs_setKeyNum(u8(value)); break;
        case kCounter_MaxMagic:    dComIfGs_setMaxMagic(u8(value)); break;
        case kCounter_ArrowNum:    dComIfGs_setArrowNum(u8(value)); break;
        case kCounter_PachinkoNum: dComIfGs_setPachinkoNum(u8(value)); break;
        case kCounter_MaxOil:      dComIfGs_setMaxOil(u16(value)); break;
        case kCounter_WalletSize:  dComIfGs_setWalletSize(u8(value)); break;
        default: break;
    }
}

}  // namespace

bool IsApplyingFromPeer() { return t_applyingFromPeer; }

// ---------------------------------------------------------------------------
// Admin / status API (called from the Settings UI on the game thread)
// ---------------------------------------------------------------------------

const char* GetStateName() {
    switch (internal::G().state.load()) {
        case internal::State::Idle:         return "Idle";
        case internal::State::Searching:    return "Searching";
        case internal::State::Handshaking:  return "Handshaking";
        case internal::State::Connected:    return "Connected";
        case internal::State::Disconnected: return "Disconnected";
    }
    return "?";
}

uint16_t GetSelfPort() { return internal::G().selfPort; }
uint16_t GetPeerPort() { return internal::G().peerPort; }
uint64_t GetPeerUuid() { return internal::G().peerUuid; }

float GetMessageRateHz() {
    // Sample the in+out counters once per second; return the previous-window
    // count so the value is stable for a UI render.
    auto& g = internal::G();
    using clock = std::chrono::steady_clock;
    static auto      s_lastSample = clock::now();
    static uint64_t  s_lastTotal  = 0;
    static float     s_cached     = 0.0f;

    const uint64_t total = g.msgsIn.load() + g.msgsOut.load();
    const auto now = clock::now();
    const auto dt  = std::chrono::duration<float>(now - s_lastSample).count();
    if (dt >= 1.0f) {
        s_cached     = float(total - s_lastTotal) / dt;
        s_lastTotal  = total;
        s_lastSample = now;
    }
    return s_cached;
}

void ForceDisconnect() {
    auto& g = internal::G();
    if (g.state.load() == internal::State::Idle) return;
    g.forceDisconnectRequested.store(true);
    DuskLog.info("netcoop: ForceDisconnect requested");
}

void ResendSaveSnapshot() {
    internal::G().resendSnapshotRequested.store(true);
    DuskLog.info("netcoop: ResendSaveSnapshot requested");
}

void WarpLocalToPeer() {
    auto& g = internal::G();
    if (g.state.load() != internal::State::Connected) return;
    const LinkState* peer = GetPeerLinkState();
    if (peer == nullptr) return;
    fopAc_ac_c* local = dComIfGp_getPlayer(0);
    if (local == nullptr) return;

    local->current.pos.x = peer->pos[0];
    local->current.pos.y = peer->pos[1];
    local->current.pos.z = peer->pos[2];
    local->shape_angle.y = static_cast<s16>(peer->yaw * (32768.0f / 3.14159265f));
    local->current.angle.y = local->shape_angle.y;
    DuskLog.info("netcoop: warped local → peer @ ({:.0f}, {:.0f}, {:.0f})",
                 peer->pos[0], peer->pos[1], peer->pos[2]);
}

// ---------------------------------------------------------------------------
// Controller routing
// ---------------------------------------------------------------------------

namespace {

// Connection-order list of SDL JoystickIDs. The Nth entry is "slot N".
// Touched from the SDL event pump (game thread), no lock needed.
std::vector<int32_t> g_padOrder;

}  // namespace

int GetAssignedPadSlot() {
    auto& g = internal::G();
    if (g.state.load() != internal::State::Connected) return -1;
    // Server (host, lower port) gets slot 0; client (higher port) gets slot 1.
    return g.clientRole ? 1 : 0;
}

void NotePadConnected(int32_t which) {
    // No-op if already tracked — SDL can re-emit ADDED after a reconnect.
    if (std::find(g_padOrder.begin(), g_padOrder.end(), which) == g_padOrder.end()) {
        g_padOrder.push_back(which);
        DuskLog.info("netcoop: pad {} → slot {}", which, int(g_padOrder.size() - 1));
    }
}

void NotePadDisconnected(int32_t which) {
    auto it = std::find(g_padOrder.begin(), g_padOrder.end(), which);
    if (it != g_padOrder.end()) {
        g_padOrder.erase(it);
    }
}

int SlotForPad(int32_t which) {
    for (size_t i = 0; i < g_padOrder.size(); ++i) {
        if (g_padOrder[i] == which) return int(i);
    }
    return -1;
}

bool ShouldAcceptPad(int32_t which) {
    const int assigned = GetAssignedPadSlot();
    if (assigned < 0) return true;  // solo / not connected
    return SlotForPad(which) == assigned;
}

void WarpPeerToLocal() {
    auto& g = internal::G();
    if (g.state.load() != internal::State::Connected) return;
    fopAc_ac_c* local = dComIfGp_getPlayer(0);
    if (local == nullptr) return;

    proto::WarpRequestMsg body{};
    body.pos[0] = local->current.pos.x;
    body.pos[1] = local->current.pos.y;
    body.pos[2] = local->current.pos.z;
    body.yaw    = static_cast<float>(local->shape_angle.y) * (3.14159265f / 32768.0f);
    PushSaveOut(proto::MsgType::WarpRequest, body);
    DuskLog.info("netcoop: WarpPeerToLocal sent (peer will teleport to {:.0f}, {:.0f}, {:.0f})",
                 body.pos[0], body.pos[1], body.pos[2]);
}

void BroadcastEventBit(uint16_t flag, bool on) {
    if (t_applyingFromPeer) return;
    proto::SaveBitMsg body{flag, uint8_t(on ? 1 : 0), 0};
    PushSaveOut(proto::MsgType::SaveBit, body);
}

void BroadcastCounter(uint16_t which, uint32_t value) {
    if (t_applyingFromPeer) return;
    proto::SaveCounterMsg body{which, 0, value};
    PushSaveOut(proto::MsgType::SaveCounter, body);
}

void BroadcastItem(uint8_t slot, uint8_t item, uint16_t count) {
    if (t_applyingFromPeer) return;
    proto::SaveItemMsg body{slot, item, count};
    PushSaveOut(proto::MsgType::SaveItem, body);
}

void BroadcastEquip(uint8_t which, uint8_t item) {
    if (t_applyingFromPeer) return;
    proto::SaveEquipMsg body{which, item, 0};
    PushSaveOut(proto::MsgType::SaveEquip, body);
}

namespace internal {

// Build a SaveSnapshot from current local save state. Called from the game
// thread (peer thread requests it via a flag).
void CaptureSaveSnapshot(proto::SaveSnapshotMsg* out) {
    std::memset(out, 0, sizeof(*out));
    void* eventBits = dComIfGs_getPEventBit();
    if (eventBits) std::memcpy(out->eventBits, eventBits, sizeof(out->eventBits));
    for (uint16_t i = 0; i < kCounter_LASTID && i < 16; ++i) {
        out->counters[i] = ReadCounter(i);
    }
    out->equip[kEquip_Clothes] = uint8_t(dComIfGs_getSelectEquipClothes());
    out->equip[kEquip_Sword]   = uint8_t(dComIfGs_getSelectEquipSword());
    out->equip[kEquip_Shield]  = uint8_t(dComIfGs_getSelectEquipShield());
    out->equip[kEquip_BButton] = uint8_t(dComIfGs_getBButtonItemKey());
    out->equip[kEquip_Smell]   = uint8_t(dComIfGs_getCollectSmell());
}

// Merge a received snapshot into local save state. OR event bits, max counters,
// keep local equipment (per-player).
void ApplySaveSnapshot(const proto::SaveSnapshotMsg& snap) {
    t_applyingFromPeer = true;

    // Event bits: OR — both peers' progress accumulates idempotently.
    uint8_t* local = static_cast<uint8_t*>(dComIfGs_getPEventBit());
    if (local) {
        for (size_t i = 0; i < sizeof(snap.eventBits); ++i) {
            local[i] |= snap.eventBits[i];
        }
    }

    // Counters: max wins (rupees climb, never drop; max-life climbs only).
    for (uint16_t i = 0; i < kCounter_LASTID && i < 16; ++i) {
        uint32_t mine = ReadCounter(i);
        if (snap.counters[i] > mine) WriteCounter(i, snap.counters[i]);
    }

    // Equipment is kept local — players may choose differently.
    t_applyingFromPeer = false;
}

// Drain the saveInQueue, applying each pending mutation under the re-entry
// guard. Called once per Tick on the game thread.
void DrainSaveInbound() {
    auto& g = G();
    std::vector<std::vector<uint8_t>> in;
    {
        std::lock_guard lk(g.saveInMu);
        in.swap(g.saveInQueue);
    }
    if (in.empty()) return;

    t_applyingFromPeer = true;
    for (const auto& buf : in) {
        if (buf.size() < sizeof(proto::Header)) continue;
        proto::Header h;
        std::memcpy(&h, buf.data(), sizeof(h));
        const uint8_t* body = buf.data() + sizeof(h);
        const size_t bodyLen = buf.size() - sizeof(h);

        switch (proto::MsgType(h.type)) {
            case proto::MsgType::SaveBit: {
                if (bodyLen != sizeof(proto::SaveBitMsg)) break;
                proto::SaveBitMsg m;
                std::memcpy(&m, body, sizeof(m));
                if (m.on) dComIfGs_onEventBit(m.flag);
                else      dComIfGs_offEventBit(m.flag);
                break;
            }
            case proto::MsgType::SaveCounter: {
                if (bodyLen != sizeof(proto::SaveCounterMsg)) break;
                proto::SaveCounterMsg m;
                std::memcpy(&m, body, sizeof(m));
                WriteCounter(m.which, m.value);
                break;
            }
            case proto::MsgType::SaveItem: {
                if (bodyLen != sizeof(proto::SaveItemMsg)) break;
                proto::SaveItemMsg m;
                std::memcpy(&m, body, sizeof(m));
                g_dComIfG_gameInfo.info.getPlayer().getItem().setItem(m.slot, m.item);
                // m.count reserved for future item-count syncing.
                break;
            }
            case proto::MsgType::SaveEquip: {
                if (bodyLen != sizeof(proto::SaveEquipMsg)) break;
                proto::SaveEquipMsg m;
                std::memcpy(&m, body, sizeof(m));
                switch (m.which) {
                    case kEquip_Clothes: dComIfGs_setSelectEquipClothes(m.item); break;
                    case kEquip_Sword:   dComIfGs_setSelectEquipSword(m.item);   break;
                    case kEquip_Shield:  dComIfGs_setSelectEquipShield(m.item);  break;
                    case kEquip_BButton: dComIfGs_setBButtonItemKey(m.item);     break;
                    case kEquip_Smell:   dComIfGs_setCollectSmell(m.item);       break;
                    default: break;
                }
                break;
            }
            case proto::MsgType::SaveSnapshot: {
                if (bodyLen != sizeof(proto::SaveSnapshotMsg)) break;
                proto::SaveSnapshotMsg snap;
                std::memcpy(&snap, body, sizeof(snap));
                // ApplySaveSnapshot toggles the guard itself; flip ours off so
                // the nested set sticks.
                t_applyingFromPeer = false;
                ApplySaveSnapshot(snap);
                t_applyingFromPeer = true;
                break;
            }
            case proto::MsgType::WarpRequest: {
                if (bodyLen != sizeof(proto::WarpRequestMsg)) break;
                proto::WarpRequestMsg m;
                std::memcpy(&m, body, sizeof(m));
                fopAc_ac_c* local = dComIfGp_getPlayer(0);
                if (local != nullptr) {
                    local->current.pos.x = m.pos[0];
                    local->current.pos.y = m.pos[1];
                    local->current.pos.z = m.pos[2];
                    local->shape_angle.y = static_cast<s16>(m.yaw * (32768.0f / 3.14159265f));
                    local->current.angle.y = local->shape_angle.y;
                    DuskLog.info("netcoop: peer warped us to ({:.0f}, {:.0f}, {:.0f})",
                                 m.pos[0], m.pos[1], m.pos[2]);
                }
                break;
            }
            case proto::MsgType::WorldLocation: {
                if (bodyLen != sizeof(proto::WorldLocationMsg)) break;
                proto::WorldLocationMsg m;
                std::memcpy(&m, body, sizeof(m));
                if (m.stage[0] == 0) break;  // host wasn't in a stage yet

                // Same stage → just warp the local Link to host's position.
                // Different stage → request an engine stage transition. The
                // play scene picks up isEnableNextStage on its next iteration
                // and loads the new stage; the local Link respawns at the
                // requested spawn point, then drifts toward host's exact pos
                // when the next steady-state WarpRequest or LinkState arrives.
                const char* current = dComIfGp_getStartStageName();
                char stage[9] = {};
                std::strncpy(stage, m.stage, 8);
                if (current != nullptr && std::strncmp(current, stage, 8) == 0) {
                    fopAc_ac_c* local = dComIfGp_getPlayer(0);
                    if (local != nullptr) {
                        local->current.pos.x = m.pos[0];
                        local->current.pos.y = m.pos[1];
                        local->current.pos.z = m.pos[2];
                        local->shape_angle.y =
                            static_cast<s16>(m.yaw * (32768.0f / 3.14159265f));
                        local->current.angle.y = local->shape_angle.y;
                    }
                    DuskLog.info(
                        "netcoop: WorldLocation in-stage warp to ({:.0f},{:.0f},{:.0f})",
                        m.pos[0], m.pos[1], m.pos[2]);
                } else {
                    DuskLog.info(
                        "netcoop: WorldLocation cross-stage handoff → {} room {} pt {}",
                        stage, m.roomNo, m.point);
                    dComIfGp_setNextStage(stage, m.point, m.roomNo, m.layer);
                }
                break;
            }
            default: break;
        }
    }
    t_applyingFromPeer = false;
}

}  // namespace internal

void Shutdown() {
    auto& g = internal::G();
    if (!internal::g_worker.joinable()) return;
    g.shutdownRequested.store(true);

    // Force-close sockets so any blocking recv unblocks.
    ws::Close(&g.peerSock);
    ws::Close(&g.listenSock);

    internal::g_worker.join();
    ws::GlobalShutdown();
    g.state.store(internal::State::Idle);
    DuskLog.info("netcoop: Shutdown complete");
}

bool IsActive() {
    return internal::G().state.load() == internal::State::Connected;
}

void SetLocalLinkState(const LinkState& s) {
    auto& g = internal::G();
    std::lock_guard lk(g.outMu);
    g.localState      = s;
    g.localStateDirty = true;
}

const LinkState* GetPeerLinkState() {
    auto& g = internal::G();
    std::lock_guard lk(g.inMu);
    return g.peerStateValid ? &g.peerState : nullptr;
}

}  // namespace dusk::netcoop

#endif  // DUSK_NETCOOP
