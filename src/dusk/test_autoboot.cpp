// See dusk/test_autoboot.hpp for the rationale + env-var contract.

#include "dusk/test_autoboot.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "dusk/logging.h"
#include "d/d_com_inf_game.h"
#include "d/d_event.h"

namespace dusk::test_autoboot {

namespace {

// Cache getenv() results so a per-frame call doesn't repeatedly hit the
// libc lookup. The value is read once on first query and frozen — toggling
// the env var after launch has no effect (matches every other process-env
// based test knob).
bool cached_bool_env(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && v[0] != '\0' && v[0] != '0';
}

}  // namespace

bool fast_boot_enabled() {
    static const bool v = cached_bool_env("DUSK_TEST_FAST_BOOT");
    return v;
}

bool autoskip_enabled() {
    static const bool v = cached_bool_env("DUSK_TEST_AUTOSKIP_CUTSCENES");
    return v;
}

// True once the engine has loaded F_SP103 (Ordon Village exterior) — the
// stick-injection probe waits for this so we don't move Link during the
// autoboot's intervening stages (F_SP102 menu scaffolding, F_SP108 intro
// cutscene where stick input would race the cutscene-skip Start presses).
static bool s_reachedTargetStage = false;

namespace {

float cached_float_env(const char* name) {
    const char* v = std::getenv(name);
    if (v == nullptr || v[0] == '\0') {
        return std::numeric_limits<float>::quiet_NaN();
    }
    try {
        float f = std::strtof(v, nullptr);
        if (f < -1.0f) f = -1.0f;
        if (f >  1.0f) f =  1.0f;
        return f;
    } catch (...) {
        return std::numeric_limits<float>::quiet_NaN();
    }
}

}  // namespace

float stick_x() {
    static const float v = cached_float_env("DUSK_TEST_STICK_X");
    return v;
}

float stick_y() {
    static const float v = cached_float_env("DUSK_TEST_STICK_Y");
    return v;
}

bool stick_enabled() {
    // Only inject once Link is in the target gameplay stage. Stick input
    // during the autoboot's earlier stages would race the cutscene-skip
    // Start synthesis and probably leave the autoboot in a weird state.
    if (!s_reachedTargetStage) return false;
    return !std::isnan(stick_x()) || !std::isnan(stick_y());
}

void on_stage_change(const char* stage, int point, int room, int layer) {
    if (!fast_boot_enabled()) return;
    if (stage == nullptr) stage = "";
    DuskLog.info("test: stage-change to={} point={} room={} layer={}",
                 stage, point, room, layer);
    if (std::strcmp(stage, "F_SP103") == 0) {
        s_reachedTargetStage = true;
    }
}

void tick() {
    if (!fast_boot_enabled()) return;

    // Rising-edge detect "events just finished while we're in a real stage."
    // Emits at most once per transition; harness waits_for this combined with
    // the prior stage-change to know Link is interactive.
    static bool s_wasRunning = false;
    dEvt_control_c* evt = dComIfGp_getEvent();
    if (evt == nullptr) {
        s_wasRunning = false;
        return;
    }

    const bool nowRunning = evt->runCheck();
    if (s_wasRunning && !nowRunning) {
        const char* stage = dComIfGp_getStartStageName();
        if (stage == nullptr) stage = "";
        DuskLog.info("test: events-idle stage={}", stage);

        // First time we settle into F_SP103, log the local Link's spawn
        // position so the harness has a known baseline for "Link has moved
        // away from where he started" assertions. Idempotent — only fires
        // the first time.
        static bool s_loggedLocalSpawn = false;
        if (!s_loggedLocalSpawn && std::strcmp(stage, "F_SP103") == 0) {
            fopAc_ac_c* local = dComIfGp_getPlayer(0);
            if (local != nullptr) {
                DuskLog.info("test: local-spawn @ ({:.1f}, {:.1f}, {:.1f})",
                             local->current.pos.x, local->current.pos.y,
                             local->current.pos.z);
                s_loggedLocalSpawn = true;
            }
        }
    }
    s_wasRunning = nowRunning;
}

}  // namespace dusk::test_autoboot
