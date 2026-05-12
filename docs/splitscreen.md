# Splitscreen Co-op — Implementation Plan

Status: **scaffolding / pre-implementation**. Target branch: `feat/splitscreen` on `sh1ftmaker/dusk-splitscreen`. Upstream: `TwilitRealm/dusk@main`.

## 1. Goals & non-goals

**Goals**

- Local two-player splitscreen co-op. P1 always present; P2 is the joiner.
- P2 can drop in and drop out at any time during gameplay (overworld and most rooms).
- P2 can teleport to P1 on demand via a button combo (also used automatically on room transition).
- Inventory and progression are shared between players (single source of truth: the existing save struct).
- No save file format changes — splitscreen is transient state.
- Compile-time opt-in via CMake (`-DDUSK_SPLITSCREEN=ON`). Off by default; zero impact on single-player builds when off.

**Non-goals (for the first cut)**

- Online / netplay. Local-only.
- More than two players. Engine pad slots support 4, but the camera/viewport plan is 2-up only.
- Splitscreen during cutscenes, intro, end credits, or boss intro sequences — P2 is hidden / parked during those; rejoins after.
- Changes to canonical save format. P2 progress is folded into the shared save on save events.

## 2. Engine facts we are relying on

Verified from the survey (file:line references):

- Player getter is **already index-based**: `g_dComIfG_gameInfo.play.getPlayer(int idx)` at `include/d/d_com_inf_game.h:583`. Only idx 0 is currently spawned.
- Player actor: `daAlink_c : public daPy_py_c` at `include/d/actor/d_a_alink.h:227`, impl `src/d/actor/d_a_alink.cpp`.
- Per-frame entry: `fapGm_Execute()` at `src/f_ap/f_ap_game.cpp:815`. PC path runs `fapGm_Before` → `duskExecute` → `fapGm_After` (`f_ap_game.cpp:727`).
- Camera manager: `fopCamM_Create(int i_cameraIdx, …)` at `src/f_op/f_op_camera_mng.cpp:18` with a 4-slot table `l_fopCamM_id[4]`.
- Input: `mDoCPd_c::m_gamePad[4]` at `src/m_Do/m_Do_controller_pad.cpp:15`. `read()` polls all four. Pads 1–3 currently only used in debug.
- Viewport / framebuffer: `GXSetViewport` / `GXSetScissor` in `src/m_Do/m_Do_graphic.cpp` (`FB_WIDTH`, `FB_HEIGHT` constants).
- Save: `dSv_info_c` and `dSv_player_status_a_c` in `include/d/d_save.h`. Item slots = 24, equip slots = 6.

These are the load-bearing assumptions. If any turn out to be wrong during step 0, the plan needs to be revised before code lands.

## 3. Architecture overview

We add a small splitscreen subsystem that owns:

1. The lifecycle of player index 1 (spawn, despawn, warp).
2. The lifecycle of camera index 1 (create, retarget, destroy).
3. Per-frame viewport selection so the renderer runs twice with two scissor rects.
4. Input routing — pad 1 events go to player 1 only.

We do **not** duplicate save data. We do **not** duplicate inventory. P2 reads/writes the same `dSv_info_c` that P1 does; "shared" is the natural state if we leave the save alone.

### 3.1 Files we expect to add

```
include/dusk/splitscreen.hpp         — public API (init, tick, drop-in/out, teleport)
src/dusk/splitscreen.cpp             — owns P2 lifecycle + viewport state
src/dusk/splitscreen_render.cpp      — per-eye render pass scaffolding
src/dusk/splitscreen_input.cpp       — pad 1 binding / drop-in button watcher
docs/splitscreen.md                  — this document
```

### 3.2 Files we expect to modify

| File | Why |
|---|---|
| `CMakeLists.txt` | Add `DUSK_SPLITSCREEN` option, link new sources |
| `files.cmake` | Register the four new translation units |
| `src/dusk/main.cpp` | Init/shutdown hook for splitscreen subsystem |
| `src/f_ap/f_ap_game.cpp` | Call `dusk_ss::Tick()` inside `fapGm_Execute`; wrap render in two-pass loop when active |
| `src/m_Do/m_Do_graphic.cpp` | Accept a viewport rect from splitscreen and set `GXSetViewport`/`GXSetScissor` accordingly |
| `src/m_Do/m_Do_controller_pad.cpp` | Always-on read for pad 1 when splitscreen flag is set (currently debug-only) |
| `src/f_op/f_op_camera_mng.cpp` | Allow camera 1 to be created/destroyed at runtime (not just on stage load) |
| `src/d/actor/d_a_alink.cpp` | Read pad slot from `mPlayerInfo[idx].pad_id` instead of always pad 0 |
| `include/d/d_com_inf_game.h` | Add a `pad_id` and `is_active` per `mPlayerInfo[idx]` |

## 4. Phased delivery

Each phase ends with a runnable build. The single-player path must never regress, regardless of whether `DUSK_SPLITSCREEN=ON`.

### Phase 0 — Verify assumptions (no code)

- Open the eight files in the table above and confirm the survey's claims at each cited line.
- Build upstream `main` clean on this machine. Document the build command in `docs/building.md` if anything Windows-specific bites.
- Decide: do we want our work to ride on the existing `wasm-port` branch in `dusk-wasm`, or on upstream `main`? (Currently planned: `main`.)

Exit criteria: clean upstream build, all eight assumptions confirmed or plan revised.

### Phase 1 — CMake flag + dead subsystem

- Add `option(DUSK_SPLITSCREEN "Enable two-player local splitscreen" OFF)`.
- Add the four new TUs (empty stubs).
- `dusk_ss::Init()` / `Shutdown()` called from `main.cpp` behind `#ifdef DUSK_SPLITSCREEN`.
- `dusk_ss::Tick()` called from `fapGm_Execute` — no-op for now.

Exit criteria: builds with and without the flag. Single-player runs identically.

### Phase 2 — Second player actor

- Add `mPlayerInfo[1].pad_id` (default 1) and `mPlayerInfo[1].is_active` (default false).
- Implement `dusk_ss::SpawnPlayer2(pos)`: calls `fopAcM_create` for `daAlink_c` at index 1.
- Implement `dusk_ss::DespawnPlayer2()`.
- Modify `daAlink_c` input read to honor `mPlayerInfo[idx].pad_id`.
- Debug keybind: F9 spawns P2 at P1's position + (1m, 0, 0); F10 despawns.

Exit criteria: two Links visible on one camera, both controllable from separate pads, neither softlocks the other.

### Phase 3 — Second camera + viewport split

- Implement `dusk_ss::ActivateCamera2()`: `fopCamM_Create(1, …)` bound to player 1.
- Modify `fapGm_After` so when splitscreen is active, scene render runs twice: once per eye, with a viewport/scissor of (0, 0, W/2, H) and (W/2, 0, W/2, H). Each pass uses its respective camera.
- Confirm Aurora's GX layer tolerates two scissored passes per swapchain frame. If not, this phase blocks on an Aurora upstream change or a workaround pass.
- HUD: only draw HUD for the active eye's player on each pass. Hearts and rupees show on both eyes (shared pool); equipped items show per-eye.

Exit criteria: two viewports, two cameras, each follows its own Link.

### Phase 4 — Drop-in / drop-out

- Watch pad 1 for "+" / Start on every frame. On press while P2 inactive: spawn at P1's position + lateral offset, fade in, activate camera 1, transition viewport from full to split over 12 frames.
- On pad 1 Start + Select held 1s while P2 active, or on pad 1 disconnect event: despawn P2, destroy camera 1, transition viewport back to full over 12 frames.
- Pad 1 disconnect mid-game must not freeze the simulation — P2 actor becomes dormant for a grace window, then is despawned.

Exit criteria: P2 can join and leave from gameplay without crashing or corrupting actor state. Pad hot-unplug handled.

### Phase 5 — Teleport-to-P1

- Button combo on pad 1 (proposed: hold L+R+D-Pad-Up for 0.5s) calls `dusk_ss::WarpPlayer2ToPlayer1()`:
  1. Read P1 transform.
  2. Find a free spawn offset on a navmesh-walkable cell within 2m of P1 (fallback: P1 position exactly, accept overlap).
  3. Set P2 transform; clear P2 velocity; reset state machine to idle.
  4. If P1 is in a different room than P2 (possible during room transition lag), recreate P2 in P1's room.

- Automatic teleport on room/stage transition: when P1 crosses a loadzone, queue a warp for P2 to fire after the new stage is loaded. P2 is hidden (dormant actor) during the transition.

Exit criteria: warp combo works on flat ground, on slopes, near walls, and during/after a loadzone.

### Phase 6 — Shared progression semantics

- Rupees, ammo (arrows, bombs, seeds), keys, map/compass, story flags, heart containers, wallet, quiver: **shared**. Single read/write to `dSv_info_c`.
- Equipped items per Link: **per-player runtime state**, drawn from the shared pool. P2 picks Sword + Shield by default if available; otherwise unarmed.
- Hearts (current HP): **per-player**. Each Link has its own HP. Death of one player → respawn at the other after 5s (bubble-revive style), no game over unless both are down simultaneously.
- Game over only fires when both players are KO'd.
- Save events (write to disc): only P1's transform is canonical; the inventory write is already a no-op of shared state.

Exit criteria: picking up a rupee with P2 increments the shared counter; using a key with P1 deducts the same key P2 sees; story flag set by either Link advances both.

### Phase 7 — Edge cases & polish

- Cutscenes: viewport collapses to full-screen on P1's camera; P2 is parked (dormant) during the cutscene and warped to P1 on exit.
- Boss rooms: usually fine; but if the boss spawn cutscene snaps the camera, follow Phase 6's parking rule.
- Pause menu: opening the menu pauses both players. Inventory menu shows the shared inventory; equip changes apply to whichever player opened it.
- Wolf-Link / transformation states: P2 inherits P1's allowed forms (story-gated), but transformation is per-player. P1 going wolf does not force P2 to.
- Camera collision: each eye's camera resolves independently; expect occasional jank where P2's camera clips through geometry P1's doesn't — accept for v1.

## 5. Risks & open questions

1. **Aurora two-pass rendering.** The biggest unknown. We need to confirm the abstraction allows two GX passes with different viewports/scissors into one swapchain image. If it forces a flush per pass, perf may bite. Mitigation: prototype in Phase 3; if blocked, fall back to rendering each eye to an offscreen RT and compositing.
2. **Actor system assumptions about "the player".** Plenty of code likely calls `getPlayer(0)` directly. Each such site needs to be reviewed for whether it should be the local-camera's player or a specific player. Mitigation: grep `getPlayer(0)` and audit; provide `getPlayer(currentRenderEye)` accessor for render-time queries.
3. **Stage streaming.** When P1 and P2 are in different rooms briefly (transition window), entity references may be invalidated. Mitigation: park P2 as dormant during the transition; warp on completion.
4. **Save corruption.** If P2's transient state ever leaks into `dSv_info_c` in an inconsistent way (e.g. mid-write disconnect), the save could be partially updated. Mitigation: write-on-P1's-save-event only; ignore P2 transform completely on save.
5. **Input device ID stability.** On Windows, SDL/Aurora may renumber pads on hotplug. Mitigation: bind by device GUID once at drop-in and survive renumbering.
6. **HUD real estate.** Each half-screen is ~50% width. HUD layout may need a compact splitscreen variant. Mitigation: a `dusk_ss::IsActive()`-aware HUD path.
7. **Performance.** Double the camera, double the draw calls. Mitigation: profile in Phase 3; if needed, reduce shadow/reflection quality in splitscreen.

## 6. Decisions still owed by the user

- Heart pool: **separate** (planned) vs. shared.
- Drop-in button: **pad 1 Start** (planned). OK?
- Teleport combo: **L+R+D-Up held 0.5s** (planned). OK?
- Player 2 visual identity: same Link model? Recolor? Use Wolf Link as P2? (Affects how much asset work is in scope.)
- Branch base: **upstream `main`** (planned) vs. ride on top of the `wasm-port` work in `dusk-wasm`.

## 7. Out of scope (filed for later)

- Netplay / online co-op.
- 3+ player.
- Splitscreen on iOS/Android touch builds.
- Co-op-specific puzzle redesigns (some single-Link puzzles will be trivial with two Links; we accept this for v1).
