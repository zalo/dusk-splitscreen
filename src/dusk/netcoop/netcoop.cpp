#ifdef DUSK_NETCOOP

#include "dusk/netcoop.hpp"
#include "internal.hpp"
#include "dusk/logging.h"

#include <chrono>
#include <random>
#include <thread>

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

void Tick() {
    auto& g = internal::G();
    if (g.state.load() != internal::State::Connected) return;

    // Once per ~6 seconds, sample-log the peer's position so two-instance runs
    // can be eyeballed in the log without spamming.
    static uint64_t s_tickCounter = 0;
    if ((++s_tickCounter % 360) == 0) {
        const LinkState* peer = GetPeerLinkState();
        if (peer) {
            DuskLog.info("netcoop: peer @ ({:.1f}, {:.1f}, {:.1f}) yaw={:.2f} frame={}",
                         peer->pos[0], peer->pos[1], peer->pos[2],
                         peer->yaw, peer->serverFrame);
        }
    }
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
