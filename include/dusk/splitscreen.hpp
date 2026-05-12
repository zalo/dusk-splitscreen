#pragma once

#include "SSystem/SComponent/c_xyz.h"

class fopAc_ac_c;

// Splitscreen co-op subsystem.
//
// Compile-time gated by DUSK_SPLITSCREEN. When the flag is off, every public
// function is an inline no-op so callers see zero overhead and the single-
// player code path is bit-identical.
//
// See docs/splitscreen.md for the phased plan.

namespace dusk_ss {

struct ViewportRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

#ifdef DUSK_SPLITSCREEN

// --- Lifecycle ---------------------------------------------------------------
void Init();
void Shutdown();
void Tick();                       // call inside fapGm_Execute

// --- State queries -----------------------------------------------------------
bool IsEnabled();                  // build-time flag is on
bool IsActive();                   // P2 is joined and not dormant
bool IsTransitioning();            // joining/leaving animation in progress
int  GetActiveEye();               // 0=left, 1=right, -1=outside per-eye pass
int  GetPlayerCount();             // 1 or 2
float GetTransitionT();            // 0..1 lerp progress for join/leave anim

// --- Player lifecycle --------------------------------------------------------
void RequestJoinP2();              // edge-trigger drop-in (deferred to safe frame)
void RequestLeaveP2();             // edge-trigger drop-out
bool SpawnPlayer2(const cXyz& pos, int room_no);
void DespawnPlayer2();
void ParkPlayer2();                // cutscene/loadzone: hide + freeze
void UnparkPlayer2();

// --- Warp --------------------------------------------------------------------
void WarpPlayer2ToPlayer1();
void QueueRoomTransitionWarp();    // armed when P1 crosses a loadzone

// --- Camera ------------------------------------------------------------------
void ActivateCamera2();            // fopCamM_Create(1, ...)
void DestroyCamera2();

// --- Render ------------------------------------------------------------------
ViewportRect GetEyeViewport(int eye);    // pixel rect for left/right eye
void         SetActiveEye(int eye);      // tag the current pass
void         RenderEye(int eye);         // invoked from the two-pass loop

// --- Input -------------------------------------------------------------------
void OnInputFrame();                     // poll combos / drop-in / disconnect
int  GetPadForPlayer(int player_idx);    // index into mDoCPd_c::m_gamePad
int  GetPadForActor(fopAc_ac_c* actor);  // pad index for a specific Link actor
bool IsTeleportComboHeld(float* progress_out);

// --- Spawn-time hooks (called from daAlink_c::create) -----------------------
bool IsSpawningP2();
void RegisterNewLink(fopAc_ac_c* link, bool is_p2);
fopAc_ac_c* GetP2Actor();

// --- AI targeting helpers ---------------------------------------------------
// Returns the closer of {P1, P2} to a given world position. Falls through to
// P1 when P2 is absent / dormant. Use this from enemy AI in place of
// dComIfGp_getPlayer(0) when the intent is "where's the player I should chase".
fopAc_ac_c* GetNearestPlayer(const cXyz& from);
fopAc_ac_c* GetNearestPlayerToActor(const fopAc_ac_c* who);

// Macro form: AI_TARGET_FOR(self_actor_ptr) — resolves to the nearest player
// to `self` when splitscreen is on, or to P1 when off. Enemy files use this
// in place of `dComIfGp_getPlayer(0)` so the migration is one mechanical sed
// per file and the off-build is bit-identical to upstream.
#define AI_TARGET_FOR(actor) (::dusk_ss::GetNearestPlayerToActor(actor))

// --- Shared progression (Phase 6) -------------------------------------------
// All inventory/story flags/rupees/ammo/keys are shared by virtue of both Links
// reading/writing the same dSv_info_c globals — no API needed.
// HP is per-player; the engine's life count tracks P1, we shadow P2 here.
int  GetP1Life();             // current quarter-hearts for P1 (== engine life)
int  GetP2Life();             // current quarter-hearts for P2
void SetP2Life(int qh);
int  GetMaxLife();            // shared max (heart containers + pieces)
void OnPlayerDamage(int player_idx, int qh);   // qh = quarter-hearts of damage
bool IsBothPlayersKO();        // true → fire game over
bool IsBubbleReviveActive(int player_idx);     // KO'd Link waiting for partner
void TickReviveTimers();                       // called from Tick

// --- Edge cases (Phase 7) ----------------------------------------------------
bool IsPaused();               // pause menu is up

// --- UI / hotplug ------------------------------------------------------------
const char* GetStateName();          // "Inactive" / "Joining" / "Active" / ...
bool        IsAutoJoinOnConnect();   // true → pad-2 hotplug auto-joins
void        SetAutoJoinOnConnect(bool enable);

#else  // ---- DUSK_SPLITSCREEN disabled: inline no-ops -----------------------

inline void Init() {}
inline void Shutdown() {}
inline void Tick() {}

inline bool IsEnabled() { return false; }
inline bool IsActive() { return false; }
inline bool IsTransitioning() { return false; }
inline int  GetActiveEye() { return -1; }
inline int  GetPlayerCount() { return 1; }
inline float GetTransitionT() { return 0.0f; }

inline void RequestJoinP2() {}
inline void RequestLeaveP2() {}
inline bool SpawnPlayer2(const cXyz&, int) { return false; }
inline void DespawnPlayer2() {}
inline void ParkPlayer2() {}
inline void UnparkPlayer2() {}

inline void WarpPlayer2ToPlayer1() {}
inline void QueueRoomTransitionWarp() {}

inline void ActivateCamera2() {}
inline void DestroyCamera2() {}

inline ViewportRect GetEyeViewport(int) { return {}; }
inline void         SetActiveEye(int) {}
inline void         RenderEye(int) {}

inline void OnInputFrame() {}
inline int  GetPadForPlayer(int) { return 0; }   // PAD_1 == 0 in this codebase
inline int  GetPadForActor(fopAc_ac_c*) { return 0; }
inline bool IsTeleportComboHeld(float* p) { if (p) *p = 0.0f; return false; }

inline bool IsSpawningP2() { return false; }
inline void RegisterNewLink(fopAc_ac_c*, bool) {}
inline fopAc_ac_c* GetP2Actor() { return nullptr; }
inline fopAc_ac_c* GetNearestPlayer(const cXyz&) { return nullptr; }
inline fopAc_ac_c* GetNearestPlayerToActor(const fopAc_ac_c*) { return nullptr; }

// Off-path: caller must have `d_com_inf_game.h` already in scope (all enemy
// files do). `(void)(actor)` silences unused-variable warnings.
#define AI_TARGET_FOR(actor) ((void)(actor), dComIfGp_getPlayer(0))

inline int  GetP1Life() { return 0; }
inline int  GetP2Life() { return 0; }
inline void SetP2Life(int) {}
inline int  GetMaxLife() { return 0; }
inline void OnPlayerDamage(int, int) {}
inline bool IsBothPlayersKO() { return false; }
inline bool IsBubbleReviveActive(int) { return false; }
inline void TickReviveTimers() {}

inline bool IsPaused() { return false; }
inline const char* GetStateName() { return "Disabled"; }
inline bool IsAutoJoinOnConnect() { return false; }
inline void SetAutoJoinOnConnect(bool) {}

#endif  // DUSK_SPLITSCREEN

}  // namespace dusk_ss
