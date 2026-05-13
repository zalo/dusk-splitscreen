#ifdef DUSK_NETCOOP

#include "dusk/netcoop.hpp"
#include "internal.hpp"
#include "dusk/logging.h"

#include <chrono>
#include <random>
#include <thread>

#include "f_op/f_op_actor.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include "d/d_com_inf_game.h"
#include "SSystem/SComponent/c_xyz.h"
#include "SSystem/SComponent/c_sxyz.h"

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
        g.state.store(State::Idle);

        // Backoff before another full discovery sweep.
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    ws::Close(&g.listenSock);
    ws::Close(&g.peerSock);
}

}  // namespace
}  // namespace internal

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
    fpc_ProcID pid = fopAcM_create(
        fpcNm_ALINK_e,
        /*parameters=*/0,
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

    internal::MaybeDespawnGhost();
    internal::MaybeSpawnGhost();

    if (g.state.load() != internal::State::Connected) return;

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
