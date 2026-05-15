// See dusk/test_autoboot.hpp for the rationale + env-var contract.

#include "dusk/test_autoboot.hpp"

#include <cstdlib>
#include <cstring>

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

void on_stage_change(const char* stage, int point, int room, int layer) {
    if (!fast_boot_enabled()) return;
    if (stage == nullptr) stage = "";
    DuskLog.info("test: stage-change to={} point={} room={} layer={}",
                 stage, point, room, layer);
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
    }
    s_wasRunning = nowRunning;
}

}  // namespace dusk::test_autoboot
