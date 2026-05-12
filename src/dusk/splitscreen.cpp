#include "dusk/splitscreen.hpp"

#ifdef DUSK_SPLITSCREEN

#include "dusk/logging.h"
#include "d/d_com_inf_game.h"
#include "d/d_meter2_info.h"
#include "d/d_bg_s.h"
#include "d/d_bg_s_gnd_chk.h"
#include "f_op/f_op_actor_mng.h"
#include "f_op/f_op_camera_mng.h"
#include "f_pc/f_pc_name.h"
#include "f_pc/f_pc_manager.h"
#include "m_Do/m_Do_controller_pad.h"
#include "SSystem/SComponent/c_malloc.h"
#include "SSystem/SComponent/c_m3d.h"

#include "d/actor/d_a_alink.h"

#include <unordered_map>

namespace dusk_ss {

namespace {

enum class State {
    Inactive,   // single-player; P2 slot vacant
    Joining,    // drop-in transition (12-frame viewport animation)
    Active,     // both players present
    Leaving,    // drop-out transition
    Dormant,    // P2 actor parked during cutscene/loadzone
};

struct {
    bool   initialized      = false;
    State  state            = State::Inactive;
    int    transition_frame = 0;          // counts up during Joining/Leaving
    int    active_eye       = -1;         // 0=left, 1=right, -1=outside pass
    bool   warp_queued      = false;      // armed when P1 crosses loadzone
    fpc_ProcID p2_proc_id   = 0;
    fpc_ProcID cam2_proc_id = 0;
    bool   spawning_p2      = false;      // true during the P2 create() call
    fopAc_ac_c* p2_actor    = nullptr;
} g;

// Actor → pad index registry. Used by daAlink_c (via LINK_PAD macro) to read
// the right port. Cleared on despawn so re-entry doesn't leak entries.
std::unordered_map<fopAc_ac_c*, int> g_actor_pad_map;

constexpr int kTransitionFrames = 12;     // §4 Phase 4 drop-in animation length

// --- Phase 6 shared progression state ---------------------------------------
// HP is per-player. Engine's life counter (dComIfGs_getLife) remains P1's
// canonical HP. P2's HP shadows here. Both pull from shared dComIfGs_getMaxLife.
constexpr int kBubbleReviveFrames = 60 * 5;   // 5 seconds @ 60 fps

struct {
    int  p2_life            = 0;        // quarter-hearts; init from max on spawn
    int  p1_revive_timer    = 0;        // counts down while KO'd, partner can revive
    int  p2_revive_timer    = 0;
    bool game_over_pending  = false;
} p6;

}  // namespace

// =============================================================================
// Lifecycle
// =============================================================================

void Init() {
    if (g.initialized) return;
    g.initialized = true;
    g.state = State::Inactive;
    DuskLog.info("splitscreen: subsystem initialized; P2 inactive by default");
}

void Shutdown() {
    if (!g.initialized) return;
    if (g.state != State::Inactive) {
        DespawnPlayer2();
    }
    g_actor_pad_map.clear();
    g.initialized = false;
    DuskLog.info("splitscreen: subsystem shutdown");
}

// =============================================================================
// Phase 7 — edge cases
// =============================================================================

namespace {

// Cutscene / event auto-park: when a cutscene begins, park P2; on exit, queue
// a warp so P2 rejoins P1 at the cutscene-end position.
bool g_was_in_event_last_frame = false;

void Phase7_HandleEventTransitions() {
    // Ordinary NPC dialog / minor events: P2 keeps moving (handled by the
    // dComIfGp_event_runCheckForActor() dispatch in daAlink). We no longer
    // auto-park P2 on every event boundary because that froze P2 every time
    // P1 talked to an NPC.
    //
    // For "heavy" cutscenes (full demo with camera takeover) we still want to
    // park P2 so they don't wander off-screen of the cutscene camera. We
    // approximate "heavy" by checking dComIfGp_event_getMode() — modes other
    // than the basic talk-event imply demo-level events. This is a heuristic;
    // refine when concrete cutscene-vs-dialog signal is identified.
    const bool in_event = dComIfGp_event_runCheck();
    const u8 mode = in_event ? dComIfGp_event_getMode() : 0;
    const bool heavy = in_event && (mode != 0 && mode != 1);

    if (heavy && !g_was_in_event_last_frame) {
        if (g.state == State::Active) {
            DuskLog.info("splitscreen: heavy cutscene began (mode={}) — parking P2", mode);
            ParkPlayer2();
        }
    } else if (!in_event && g_was_in_event_last_frame) {
        if (g.state == State::Dormant) {
            DuskLog.info("splitscreen: cutscene ended — queueing P2 warp");
            QueueRoomTransitionWarp();
        }
    }
    g_was_in_event_last_frame = heavy;
}

}  // namespace

bool IsPaused() {
    return dMeter2Info_getPauseStatus() != 0;
}

void Tick() {
    if (!g.initialized) return;

    Phase7_HandleEventTransitions();

    // When paused, do not advance transition frames — but still poll input so
    // we can detect drop-out request from menu, and still react to disconnect.
    const bool paused = IsPaused();

    // Pad-2 hot-unplug while P2 is live → drop out gracefully.
    if (g.state == State::Active && mDoCPd_c::m_gamePad[PAD_2] == nullptr) {
        DuskLog.warn("splitscreen: pad-2 disconnected — auto drop-out");
        RequestLeaveP2();
    }

    if (paused) {
        return;   // pause halts the rest of the state machine
    }

    OnInputFrame();
    TickReviveTimers();

    // Queued warp from a room transition (Phase 5/7). When P1's room matches
    // the next-stage room and P2 is parked, fire the warp and unpark.
    if (g.warp_queued) {
        fopAc_ac_c* p1 = g_dComIfG_gameInfo.play.getPlayer(0);
        if (p1 != nullptr && g.p2_actor != nullptr) {
            WarpPlayer2ToPlayer1();
            UnparkPlayer2();
            g.warp_queued = false;
        }
    }

    switch (g.state) {
        case State::Joining:
            if (++g.transition_frame >= kTransitionFrames) {
                g.transition_frame = 0;
                g.state = State::Active;
            }
            break;
        case State::Leaving:
            if (++g.transition_frame >= kTransitionFrames) {
                g.transition_frame = 0;
                g.state = State::Inactive;
                DespawnPlayer2();
                DestroyCamera2();
            }
            break;
        default:
            break;
    }
}

// Transition progress 0..1. Always 0 outside Joining/Leaving.
// Used by the render path to lerp viewport width and (later) by daAlink for fade.
static float TransitionProgress() {
    if (!IsTransitioning()) return (g.state == State::Active) ? 1.0f : 0.0f;
    float t = float(g.transition_frame) / float(kTransitionFrames);
    if (g.state == State::Leaving) t = 1.0f - t;
    return t;
}

float GetTransitionT() { return TransitionProgress(); }

// =============================================================================
// State queries
// =============================================================================

bool IsEnabled()       { return true; }
bool IsActive()        { return g.state == State::Active || g.state == State::Joining; }
bool IsTransitioning() { return g.state == State::Joining || g.state == State::Leaving; }
int  GetActiveEye()    { return g.active_eye; }
int  GetPlayerCount()  { return IsActive() ? 2 : 1; }

void SetActiveEye(int eye) { g.active_eye = eye; }

bool IsSpawningP2() { return g.spawning_p2; }

// =============================================================================
// Actor / pad registry
// =============================================================================

void RegisterNewLink(fopAc_ac_c* link, bool is_p2) {
    if (link == nullptr) return;
    if (is_p2) {
        g.p2_actor = link;
        g_actor_pad_map[link] = PAD_2;
        DuskLog.info("splitscreen: P2 actor registered at pad PAD_2");
    } else {
        g_actor_pad_map[link] = PAD_1;
    }
}

int GetPadForActor(fopAc_ac_c* actor) {
    if (actor == nullptr) return PAD_1;
    auto it = g_actor_pad_map.find(actor);
    return (it != g_actor_pad_map.end()) ? it->second : PAD_1;
}

fopAc_ac_c* GetP2Actor() { return g.p2_actor; }

// =============================================================================
// AI targeting helpers
// =============================================================================

fopAc_ac_c* GetNearestPlayer(const cXyz& from) {
    fopAc_ac_c* p1 = g_dComIfG_gameInfo.play.getPlayer(0);
    fopAc_ac_c* p2 = (IsActive() ? g.p2_actor : nullptr);
    if (p2 == nullptr) return p1;
    if (p1 == nullptr) return p2;

    const cXyz d1 = p1->current.pos - from;
    const cXyz d2 = p2->current.pos - from;
    const f32 dist1_sq = d1.x * d1.x + d1.y * d1.y + d1.z * d1.z;
    const f32 dist2_sq = d2.x * d2.x + d2.y * d2.y + d2.z * d2.z;
    return (dist2_sq < dist1_sq) ? p2 : p1;
}

fopAc_ac_c* GetNearestPlayerToActor(const fopAc_ac_c* who) {
    if (who == nullptr) return g_dComIfG_gameInfo.play.getPlayer(0);
    return GetNearestPlayer(who->current.pos);
}

// =============================================================================
// Player lifecycle  (Phase 2)
// =============================================================================

void RequestJoinP2() {
    if (g.state != State::Inactive) return;
    if (g.p2_actor != nullptr) return;

    fopAc_ac_c* p1 = g_dComIfG_gameInfo.play.getPlayer(0);
    if (p1 == nullptr) {
        DuskLog.warn("splitscreen: cannot join — no P1 actor");
        return;
    }
    cXyz spawn_pos = p1->current.pos;
    spawn_pos.x += 100.0f;   // 1m lateral offset (engine units are ~100/m)

    if (!SpawnPlayer2(spawn_pos, fopAcM_GetRoomNo(p1))) {
        DuskLog.warn("splitscreen: SpawnPlayer2 failed");
        return;
    }
    ActivateCamera2();
    // Bind P2 actor to camera slot 1 so the renderer's per-eye camera lookup
    // resolves correctly (m_Do_graphic.cpp two-pass loop uses camera_id == eye).
    if (g.p2_actor != nullptr) {
        g_dComIfG_gameInfo.play.setPlayerInfo(1, g.p2_actor, 1);
    }
    g.state = State::Joining;
    g.transition_frame = 0;
}

void RequestLeaveP2() {
    if (g.state != State::Active) return;
    g.state = State::Leaving;
    g.transition_frame = 0;
    // Despawn happens at end of transition (Phase 4 wiring).
}

bool SpawnPlayer2(const cXyz& pos, int room_no) {
    if (g.p2_actor != nullptr) {
        DuskLog.warn("splitscreen: SpawnPlayer2 called but P2 already exists");
        return false;
    }
    csXyz angle{};   // identity rotation; will face whatever default Link faces
    g.spawning_p2 = true;
    const fpc_ProcID pid = fopAcM_create(
        fpcNm_ALINK_e,
        /*parameters=*/0,
        &pos,
        room_no,
        &angle,
        /*scale=*/nullptr,
        /*argument=*/-1
    );
    g.spawning_p2 = false;

    if (pid == 0) {
        DuskLog.error("splitscreen: fopAcM_create returned 0 for P2");
        return false;
    }
    g.p2_proc_id = pid;
    DuskLog.info("splitscreen: P2 Link spawned at room={}", room_no);
    return true;
}

void DespawnPlayer2() {
    if (g.p2_actor != nullptr) {
        g_actor_pad_map.erase(g.p2_actor);
    }
    if (g.p2_proc_id != 0) {
        base_process_class* proc = fpcM_SearchByID(g.p2_proc_id);
        if (proc != nullptr) {
            fpcM_Delete(proc);
        }
        g.p2_proc_id = 0;
    }
    g.p2_actor = nullptr;
    g_dComIfG_gameInfo.play.setPlayer(1, nullptr);
    DuskLog.info("splitscreen: P2 despawned");
}

void ParkPlayer2()   { if (g.state == State::Active) g.state = State::Dormant; }
void UnparkPlayer2() { if (g.state == State::Dormant) g.state = State::Active; }

// =============================================================================
// Shared progression (Phase 6)
// =============================================================================

int GetMaxLife() { return dComIfGs_getMaxLife(); }
int GetP1Life()  { return dComIfGs_getLife(); }
int GetP2Life()  { return p6.p2_life; }
void SetP2Life(int qh) {
    const int mx = GetMaxLife();
    if (qh < 0) qh = 0;
    if (qh > mx) qh = mx;
    p6.p2_life = qh;
}

void OnPlayerDamage(int player_idx, int qh) {
    // qh > 0 = damage, qh < 0 = heal (matches engine convention).
    if (qh == 0) return;
    if (player_idx == 0) {
        const int cur = (int)dComIfGs_getLife();
        int next = cur - qh;
        if (next < 0) next = 0;
        const int mx = GetMaxLife();
        if (next > mx) next = mx;
        dComIfGs_setLife((u16)next);
        if (next == 0 && qh > 0) p6.p1_revive_timer = kBubbleReviveFrames;
    } else if (player_idx == 1) {
        SetP2Life(p6.p2_life - qh);
        if (p6.p2_life == 0 && qh > 0) p6.p2_revive_timer = kBubbleReviveFrames;
    }
}

bool IsBubbleReviveActive(int player_idx) {
    return player_idx == 0 ? (p6.p1_revive_timer > 0)
                           : (p6.p2_revive_timer > 0);
}

bool IsBothPlayersKO() {
    const bool p1_ko = (dComIfGs_getLife() == 0);
    const bool p2_ko = IsActive() ? (p6.p2_life == 0) : false;
    return p1_ko && (IsActive() ? p2_ko : true);
}

void TickReviveTimers() {
    // Partner-revive: if one Link is in bubble and the other is alive, count
    // down. Reaching 0 = full game over (both KO'd) or auto-revive at low HP.
    // Phase-6 polish: actually trigger the in-engine revive animation; for
    // now we just refill 1 heart and clear the timer.
    if (p6.p1_revive_timer > 0) {
        if (IsActive() && p6.p2_life > 0) {
            if (--p6.p1_revive_timer == 0) {
                dComIfGs_setLife(4);   // 1 heart = 4 quarter-hearts
            }
        } else {
            p6.game_over_pending = true;
        }
    }
    if (p6.p2_revive_timer > 0) {
        if (dComIfGs_getLife() > 0) {
            if (--p6.p2_revive_timer == 0) {
                SetP2Life(4);
            }
        } else {
            p6.game_over_pending = true;
        }
    }
}

// =============================================================================
// Warp  (Phase 5)
// =============================================================================

void WarpPlayer2ToPlayer1() {
    if (g.p2_actor == nullptr) return;

    fopAc_ac_c* p1 = g_dComIfG_gameInfo.play.getPlayer(0);
    if (p1 == nullptr) return;

    const int p1_room = fopAcM_GetRoomNo(p1);
    const int p2_room = fopAcM_GetRoomNo(g.p2_actor);

    // Spawn-offset probe: try 8 cardinal+diagonal candidates around P1, probe
    // the bg collision for ground Y, accept the first hit within ±300u of P1's
    // own Y (rules out cliffs, voids, and rooftops below).
    static constexpr float kOffsets[][2] = {
        { 100.0f,    0.0f}, {-100.0f,    0.0f},
        {   0.0f,  100.0f}, {   0.0f, -100.0f},
        {  71.0f,   71.0f}, { -71.0f,   71.0f},
        {  71.0f,  -71.0f}, { -71.0f,  -71.0f},
    };
    cXyz target = p1->current.pos;
    bool found = false;
    dBgS_GndChk gnd_chk;
    for (auto& o : kOffsets) {
        cXyz cand = p1->current.pos;
        cand.x += o[0];
        cand.z += o[1];
        // Probe from slightly above P1's head down to find first ground.
        cand.y = p1->current.pos.y + 100.0f;
        gnd_chk.SetPos(&cand);
        const f32 gy = dComIfG_Bgsp().GroundCross(&gnd_chk);
        if (gy < G_CM3D_F_INF * 0.5f && fabsf(gy - p1->current.pos.y) <= 300.0f) {
            target = cand;
            target.y = gy;
            found = true;
            break;
        }
    }
    if (!found) {
        // Fallback: exact P1 position (accept overlap).
        target = p1->current.pos;
        DuskLog.warn("splitscreen: warp probe found no walkable cell; using P1 position");
    }

    // Room mismatch (e.g. P1 already moved through a loadzone): warping
    // across rooms via direct position writes will desync the streaming
    // system. Despawn-respawn in P1's room is safer. Phase-5 polish.
    if (p1_room != p2_room) {
        DuskLog.warn("splitscreen: warp across rooms ({}→{}) — respawning P2", p2_room, p1_room);
        DespawnPlayer2();
        SpawnPlayer2(target, p1_room);
        // Re-bind camera; ActivateCamera2 is idempotent.
        ActivateCamera2();
        if (g.p2_actor != nullptr) {
            g_dComIfG_gameInfo.play.setPlayerInfo(1, g.p2_actor, 1);
        }
        return;
    }

    g.p2_actor->current.pos     = target;
    g.p2_actor->old.pos         = target;
    g.p2_actor->home.pos        = target;
    g.p2_actor->speed.zero();
    g.p2_actor->speedF          = 0.0f;
    g.p2_actor->current.angle.y = p1->current.angle.y;
    g.p2_actor->shape_angle.y   = p1->shape_angle.y;
    DuskLog.info("splitscreen: warped P2 to P1 (same room {})", p1_room);
}

void QueueRoomTransitionWarp() {
    g.warp_queued = true;
    ParkPlayer2();
    // The actual fire happens in Tick once the new stage has settled. We
    // detect "settled" cheaply by waiting for P1's room number to equal the
    // currently-started stage room; refine in Phase 7 with a proper hook into
    // dStage_setNextStage / d_s_play state transitions.
}

// =============================================================================
// Camera  (Phase 3)
// =============================================================================

void ActivateCamera2() {
    if (g.cam2_proc_id != 0) return;

    // Allocate per the same pattern d_stage.cpp uses to spawn the primary camera.
    auto* params = static_cast<fopCamM_prm_class*>(
        cMl::memalignB(-4, sizeof(fopCamM_prm_class)));
    if (params == nullptr) {
        DuskLog.error("splitscreen: failed to allocate fopCamM_prm_class for cam2");
        return;
    }
    params->base.position.x  = 0.0f;
    params->base.position.y  = 0.0f;
    params->base.position.z  = 0.0f;
    // base.parameters becomes the process's `parameters` field which the camera
    // reads back via fopCamM_GetParam == get_camera_id. Without this, the new
    // camera self-registers as slot 0 and clobbers P1's camera.
    params->base.parameters  = 1;

    g.cam2_proc_id = fopCamM_Create(/*cameraIdx=*/1, fpcNm_CAMERA_e, params);
    if (g.cam2_proc_id == 0) {
        DuskLog.error("splitscreen: fopCamM_Create for cam2 returned 0");
        return;
    }
    // Belt-and-suspenders: also set the proc's parameters field directly in
    // case the params-struct path doesn't propagate (depends on fpc internals).
    base_process_class* proc = fpcM_SearchByID(g.cam2_proc_id);
    if (proc != nullptr) {
        fpcM_SetParam(proc, 1);
    }
    DuskLog.info("splitscreen: camera slot 1 created (proc_id={})", g.cam2_proc_id);
}

void DestroyCamera2() {
    if (g.cam2_proc_id == 0) return;
    base_process_class* proc = fpcM_SearchByID(g.cam2_proc_id);
    if (proc != nullptr) fpcM_Delete(proc);
    g.cam2_proc_id = 0;
}

}  // namespace dusk_ss

#endif  // DUSK_SPLITSCREEN
