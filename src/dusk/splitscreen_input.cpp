#include "dusk/splitscreen.hpp"

#ifdef DUSK_SPLITSCREEN

#include "dusk/logging.h"
#include "d/d_com_inf_game.h"
#include "m_Do/m_Do_controller_pad.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include <cstdlib>
#include <cstring>

namespace dusk_ss {

namespace {

// PAD_1..PAD_4 here are port indices (enum at m_Do_controller_pad.h:9, values 0..3).
// In Twilight Princess parlance, PAD_1 == port 1 == m_gamePad[0] == player 1.
constexpr int kPadP1 = PAD_1;   // 0
constexpr int kPadP2 = PAD_2;   // 1

// Teleport combo: L+R+D-Up held for ~30 frames (~0.5s @ 60Hz).
constexpr int kTeleportFramesRequired = 30;

// Drop-out: Start held on pad 2 for ~60 frames (~1s) — keeps Start single-press
// available as the drop-in trigger (edge vs. level).
constexpr int kDropOutFramesRequired = 60;

// =============================================================================
// Diagnostic env vars (parsed once at startup; -1 disables)
// =============================================================================

int env_int(const char* name, int dflt) {
    const char* v = std::getenv(name);
    return v ? std::atoi(v) : dflt;
}

const int g_auto_join_frame   = env_int("DUSK_SS_AUTO_JOIN",   -1);
const int g_auto_leave_frame  = env_int("DUSK_SS_AUTO_LEAVE",  -1);
const int g_auto_warp_frame   = env_int("DUSK_SS_AUTO_WARP",   -1);
const int g_skip_intro_frame  = env_int("DUSK_SS_SKIP_INTRO",  -1);

// =============================================================================
// Frame state
// =============================================================================

struct KbState {
    bool prev[SDL_SCANCODE_COUNT] = {};

    bool pressed_edge(SDL_Scancode k, const bool* now) const {
        return now[k] && !prev[k];
    }
};

struct {
    int  teleport_hold_frames = 0;
    int  dropout_hold_frames  = 0;
    int  frame_count          = 0;
    bool intro_skipped        = false;
    KbState kb;
} s;

// Skip the intro / title sequence by jumping to the Ordon Province field area.
// Same target the name-select / menu screens use after starting a new game.
void TriggerIntroSkip() {
    if (s.intro_skipped) return;
    s.intro_skipped = true;
    DuskLog.info("splitscreen: skipping intro → F_SP108 (Ordon)");
    dComIfGp_setNextStage("F_SP108", /*point=*/21, /*roomNo=*/1, /*layer=*/13);
}

void PollKeyboard() {
    int n_keys = 0;
    const bool* keys = SDL_GetKeyboardState(&n_keys);
    if (keys == nullptr || n_keys <= 0) return;

    // F9 — request P2 join (edge-triggered).
    if (s.kb.pressed_edge(SDL_SCANCODE_F9, keys)) {
        DuskLog.info("splitscreen: F9 — request P2 join");
        RequestJoinP2();
    }
    // F10 — request P2 leave.
    if (s.kb.pressed_edge(SDL_SCANCODE_F10, keys)) {
        DuskLog.info("splitscreen: F10 — request P2 leave");
        RequestLeaveP2();
    }
    // F11 — warp P2 to P1.
    if (s.kb.pressed_edge(SDL_SCANCODE_F11, keys)) {
        DuskLog.info("splitscreen: F11 — warp P2 to P1");
        WarpPlayer2ToPlayer1();
    }
    // F12 — skip the current cutscene / intro.
    if (s.kb.pressed_edge(SDL_SCANCODE_F12, keys)) {
        DuskLog.info("splitscreen: F12 — skip intro");
        TriggerIntroSkip();
    }

    std::memcpy(s.kb.prev, keys,
                static_cast<size_t>(n_keys) * sizeof(bool));
}

}  // namespace

int GetPadForPlayer(int player_idx) {
    return player_idx == 1 ? kPadP2 : kPadP1;
}

bool IsTeleportComboHeld(float* progress_out) {
    if (progress_out) {
        *progress_out = float(s.teleport_hold_frames) / float(kTeleportFramesRequired);
    }
    return s.teleport_hold_frames >= kTeleportFramesRequired;
}

void OnInputFrame() {
    s.frame_count++;

    // -------- Env-var diagnostic triggers (frame-counted) --------
    if (g_skip_intro_frame >= 0 && s.frame_count == g_skip_intro_frame) {
        TriggerIntroSkip();
    }
    if (g_auto_join_frame >= 0 && s.frame_count == g_auto_join_frame
        && !IsActive() && GetP2Actor() == nullptr)
    {
        DuskLog.info("splitscreen: DUSK_SS_AUTO_JOIN fired at frame {}", s.frame_count);
        RequestJoinP2();
    }
    if (g_auto_warp_frame >= 0 && s.frame_count == g_auto_warp_frame
        && GetP2Actor() != nullptr)
    {
        DuskLog.info("splitscreen: DUSK_SS_AUTO_WARP fired at frame {}", s.frame_count);
        WarpPlayer2ToPlayer1();
    }
    if (g_auto_leave_frame >= 0 && s.frame_count == g_auto_leave_frame
        && IsActive())
    {
        DuskLog.info("splitscreen: DUSK_SS_AUTO_LEAVE fired at frame {}", s.frame_count);
        RequestLeaveP2();
    }

    // -------- Keyboard hotkeys (live, edge-detected) --------
    PollKeyboard();

    // -------- Pad-based triggers --------
    // Drop-in: pad 2 Start edge while no P2 present.
    if (!IsActive() && GetP2Actor() == nullptr) {
        if (mDoCPd_c::getTrigStart(kPadP2)) {
            DuskLog.info("splitscreen: pad-2 Start pressed — requesting join");
            RequestJoinP2();
        }
    }

    // Drop-out: pad 2 Start held while P2 active.
    if (IsActive()) {
        if (mDoCPd_c::getHoldStart(kPadP2)) {
            s.dropout_hold_frames++;
            if (s.dropout_hold_frames >= kDropOutFramesRequired) {
                DuskLog.info("splitscreen: pad-2 Start held — requesting leave");
                RequestLeaveP2();
                s.dropout_hold_frames = 0;
            }
        } else {
            s.dropout_hold_frames = 0;
        }
    } else {
        s.dropout_hold_frames = 0;
    }

    // Teleport-to-P1 combo on pad 1: L + R + D-Up held for 0.5s.
    const bool teleport_combo =
        IsActive()
        && mDoCPd_c::getHoldL(kPadP1)
        && mDoCPd_c::getHoldR(kPadP1)
        && mDoCPd_c::getHoldUp(kPadP1);

    if (teleport_combo) {
        s.teleport_hold_frames++;
        if (s.teleport_hold_frames == kTeleportFramesRequired) {
            DuskLog.info("splitscreen: teleport combo armed — warping P2");
            WarpPlayer2ToPlayer1();
        }
    } else {
        s.teleport_hold_frames = 0;
    }
}

}  // namespace dusk_ss

#endif  // DUSK_SPLITSCREEN
