// dusk/test_autoboot.hpp — opt-in env-var hooks that automate the boot path
// for headless / CI test runs.
//
// All entry points return false / no-op when the gating env var is unset, so
// normal player-driven boots are bit-identical to upstream.
//
//   DUSK_TEST_FAST_BOOT          — auto-confirm the title screen, auto-select
//                                  save slot 0 (creating one with default
//                                  names if the slot is empty), and bypass
//                                  the file-select UI animations.
//
//   DUSK_TEST_AUTOSKIP_CUTSCENES — every frame a demo's mSkipFunc is armed,
//                                  inject a single virtual Start press to
//                                  begin the engine's normal 45-frame skip
//                                  animation. Re-arms per cutscene.
//
// Both flags also enable a small set of [INFO | dusk] "test: ..." log lines
// (stage-change, events-idle) so a Python harness can wait_for_log() against
// a deterministic landmark instead of polling pixels or timing.

#pragma once

namespace dusk::test_autoboot {

// Cached cheaply on first call. Each query reads getenv() at most once per
// process lifetime; the result is cached in a function-local static.
bool fast_boot_enabled();
bool autoskip_enabled();

// Called once per game frame from the main loop. Watches dEvt_control_c's
// runCheck() and emits a single [INFO | dusk] test: events-idle line on the
// rising edge of "events just finished" while in a non-empty stage. Cheap;
// short-circuits to nothing when fast_boot_enabled() is false.
void tick();

// Called from d_com_inf_game's setStartStage(). Emits a stage-change log
// when fast_boot is enabled. Stays no-op otherwise.
void on_stage_change(const char* stage, int point, int room, int layer);

}  // namespace dusk::test_autoboot
