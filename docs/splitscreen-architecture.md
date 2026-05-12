# Splitscreen Co-op — Architecture

Companion to `docs/splitscreen.md` (the plan). This document describes what was
actually shipped: where the subsystem lives, what it owns, how it integrates
with the rest of the engine, and where the load-bearing patches sit.

## 1. Compile-time gate

The entire feature is gated on the `DUSK_SPLITSCREEN` CMake option (default
`OFF`). When off:

- Header public API in `include/dusk/splitscreen.hpp` resolves to inline
  no-ops, so callers compile to nothing.
- The four splitscreen translation units are excluded from the build.
- Every engine-side `#ifdef DUSK_SPLITSCREEN` block disappears, restoring the
  original code path byte-for-byte. Verified: a `DUSK_SPLITSCREEN=OFF` build
  runs identically to upstream `main`.

This means a release built without the flag pays zero binary or runtime cost.

## 2. Subsystem layout

```
include/dusk/splitscreen.hpp        ── public API in namespace dusk_ss
src/dusk/
    splitscreen.cpp                 ── lifecycle, state machine, player/cam
                                       management, warp, shared-progression
                                       routing
    splitscreen_render.cpp          ── eye viewport math (lerped during
                                       drop-in / drop-out)
    splitscreen_input.cpp           ── pad-2 trigger, teleport combo,
                                       SDL keyboard hotkeys, env-var diag
```

Everything splitscreen-specific lives in the `dusk_ss` namespace. Internal state
is hidden behind anonymous-namespace structs (`g`, `s`, `p6`, `kb`). There is
**one** owner of P2 state — the file-local `g` struct in `splitscreen.cpp`.

## 3. State machine

`splitscreen.cpp` runs a five-state FSM advanced once per game frame from
`fapGm_Execute`:

```
       ┌──────────┐  RequestJoinP2  ┌──────────┐  12 frames    ┌──────────┐
       │ Inactive │ ──────────────▶ │ Joining  │ ─────────────▶│  Active  │
       └──────────┘                 └──────────┘               └─────┬────┘
            ▲                            ▲                           │
            │                            │                  RequestLeaveP2
            │     12 frames              │                           ▼
            └────────────────── ┌──────────┐                   ┌──────────┐
                                │ Leaving  │ ◀─── pad-2 unplug │ Dormant  │
                                └──────────┘                   └──────────┘
                                                          (cutscene / loadzone)
```

- **Inactive**: single-player. P2 actor and camera 1 do not exist.
- **Joining**: P2 actor has been requested. Renderer is in two-pass mode, but
  the eye-1 viewport is interpolating from zero width to half width over 12
  frames. `IsActive()` returns true.
- **Active**: both players present, viewport fully split.
- **Leaving**: viewport interpolating back to full. Actor/camera deleted on
  state-exit.
- **Dormant**: P2 actor is hidden during a cutscene or stage loadzone. The
  renderer collapses back to single viewport; warp is queued for re-entry.

`Tick()` (`splitscreen.cpp`) advances the FSM. `GetTransitionT()` exposes a
0..1 lerp value the renderer reads each frame.

## 4. Data ownership

```
shared (one source of truth, in dSv_info_c):
    rupees, ammo (arrows / bombs / seeds), keys, map / compass, story flags,
    heart containers, wallet, quiver, item slots, equip-clothes

per-player runtime (in daAlink_c instance fields):
    pad_idx          (via the dusk_ss::g_actor_pad_map registry)
    mSelectItemId    (already a member; not modified)
    FLG1_IS_WOLF     (already a daPy_py_c flag; not modified)
    transform / pos / vel / animation state

per-player runtime (in dusk_ss::p6):
    p2_life
    p1_revive_timer / p2_revive_timer

dusk_ss::g (subsystem singleton):
    state, transition_frame, p2_proc_id, cam2_proc_id, p2_actor pointer,
    warp_queued, active_eye, spawning_p2 flag, initialized flag
```

Most "shared" state needed zero new code — both Links call the same
`dComIfGs_*` getters/setters against the single `dSv_info_c`, so picking up a
rupee with P2 automatically updates the shared counter. The only data that
required routing was current HP, because the engine has a global life counter
that's authoritative for P1 only.

## 5. Engine patches (load-bearing changes)

Eight files outside `src/dusk/` were modified. Each patch is gated on
`DUSK_SPLITSCREEN`; the non-flag build is unchanged.

| File | Patch | Why |
|---|---|---|
| `include/d/d_com_inf_game.h` | `mCameraInfo[1] → [2]` and `mPlayerInfo[1] → [2]` (with new `pad_id`, `is_active` fields per slot) | The engine had hardcoded array size 1. P2 / cam 1 had nowhere to live. |
| `src/d/d_com_inf_game.cpp` | Init loop sets `pad_id = i`, `is_active = (i == 0)` for the new slots | The loop already iterated `ARRAY_SIZE(mPlayerInfo)`, so it auto-extends |
| `src/d/actor/d_a_alink.cpp` | (a) 25 `PAD_1` input reads routed through a `LINK_PAD` macro → `dusk_ss::GetPadForActor(this)`. (b) `daAlink_c::create` slot-dispatch: if `mPlayerInfo[0]` already holds a *different* Link, this Link claims slot 1 and skips `setLinkPlayer` | Pad routing is per-actor. The slot-occupancy check is the durable signal because `IsSpawningP2()` is too short-lived (cleared before create() actually runs in the next frame) |
| `src/d/actor/d_a_alink_damage.inc` | `setDamagePoint` deducts from `dusk_ss::OnPlayerDamage(1, qh)` when the damaged actor is P2; otherwise the original `dComIfGp_setItemLifeCount(-qh, 0)` path | Per-player HP routing |
| `src/d/d_camera.cpp` | `camera_delete` uses `fopCamM_GetParam(i_this)` to find its slot instead of hardcoded 0 | **The bug that took longest to find.** The unconditional `setCamera(0, NULL)` in the delete callback was wiping P1's camera slot whenever the engine destroyed *any* camera. |
| `src/f_op/f_op_camera_mng.cpp` | `fopCamM_GetParam` does a reverse lookup against `l_fopCamM_id[]` before falling back to `fpcM_GetParam` | The proc's `parameters` field comes from the *profile* (per-procname, always 0 for camera) — both cameras would self-identify as slot 0 without this. |
| `src/m_Do/m_Do_controller_pad.cpp` | Force-create `m_gamePad[1]` (PAD_2) in non-debug builds when splitscreen is on | Pads 2-4 were only allocated under `DEBUG` or `developmentMode` |
| `src/m_Do/m_Do_graphic.cpp` | Scene render block at `:2126–2577` wrapped in `for (eye = 0; eye < eye_count; eye++)`. Per-eye camera-id and viewport selection. | The two-pass render injection |
| `src/dusk/main.cpp` | `dusk_ss::Init()`/`Shutdown()` around `game_main(...)` | Subsystem lifecycle |
| `src/f_ap/f_ap_game.cpp` | `dusk_ss::Tick()` at top of `fapGm_Execute` (PC only) | Per-frame state-machine advance |

## 6. Integration points

```
fapGm_Execute  (per-frame entry)
     │
     ├──▶ dusk_ss::Tick()
     │       ├── Phase7_HandleEventTransitions()  ── auto-park during cutscenes
     │       ├── pad-2 disconnect → RequestLeaveP2
     │       ├── if !paused:
     │       │     ├── OnInputFrame()             ── pad / keyboard / env-var
     │       │     ├── TickReviveTimers()         ── bubble-revive countdown
     │       │     ├── consume warp_queued        ── post-loadzone warp
     │       │     └── advance Joining/Leaving FSM
     │       └── (does nothing when !initialized)
     │
     ├──▶ duskExecute()  (existing Dusk frame hook)
     │
     └──▶ fpcM_Management
              │
              └── eventually: scene render in m_Do_graphic.cpp
                       │
                       └── if (dComIfGp_getWindowNum() != 0):
                              for (eye = 0; eye < eye_count; eye++):
                                   SetActiveEye(eye)
                                   camera_id = IsActive() ? eye : default
                                   GXSetViewport/Scissor ← GetEyeViewport(eye)
                                   (existing 450-line scene draw runs verbatim)
```

The renderer integration is a single `for` loop wrapped around the existing
scene block. Each pass writes into a different scissor region of the same
framebuffer — no offscreen targets, no compositing. The §5.1 risk in the plan
("Aurora may not tolerate two scissored passes per swap") tested out fine:
both passes write cleanly into their half of the swapchain image at 60 fps in
the validation run.

When `IsActive()` is false the loop runs exactly once and the path is identical
to upstream — verified by a baseline run with the flag on but no auto-join.

## 7. Player / camera lifecycle in detail

### Spawn

```
RequestJoinP2()  [state must be Inactive]
    ├── p1 = getPlayer(0); fail if null
    ├── spawn_pos = p1.pos + (100u, 0, 0)
    ├── SpawnPlayer2(spawn_pos, fopAcM_GetRoomNo(p1))
    │      ├── g.spawning_p2 = true
    │      ├── fopAcM_create(fpcNm_ALINK_e, 0, &pos, room, &angle, nullptr, -1)
    │      └── g.spawning_p2 = false; g.p2_proc_id = pid
    ├── ActivateCamera2()
    │      ├── alloc fopCamM_prm_class
    │      ├── params.base.parameters = 1   (best-effort intent)
    │      ├── fopCamM_Create(1, fpcNm_CAMERA_e, params)
    │      └── fpcM_SetParam(proc, 1)        (belt-and-suspenders)
    ├── setPlayerInfo(1, g.p2_actor, /*cam_id=*/1)
    └── state = Joining
```

Some time later (a frame or three) the process manager runs the queued procs:

```
fpcM_Management  (next frame's tick)
    └── camera 1 proc enters init_phase1
            ├── camera_id = get_camera_id(this)
            │       └── fopCamM_GetParam(this)
            │             └── reverse-lookup l_fopCamM_id[] → returns 1
            ├── dComIfGp_setCamera(1, this)   ── now mCameraInfo[1].mCamera is set
            └── ...
    └── P2 daAlink_c proc enters create()
            ├── ... (large init)
            ├── existing_p1 = getPlayer(0)
            ├── is_p2 = (existing_p1 != nullptr && existing_p1 != this)
            ├── dComIfGp_setPlayer(1, this)
            ├── dusk_ss::RegisterNewLink(this, /*is_p2=*/true)
            │      └── g.p2_actor = this; g_actor_pad_map[this] = PAD_2
            └── (skips dComIfGp_setLinkPlayer — P1 stays canonical)
```

`daAlink_c::create()` may return retry-later for several frames while resources
load; that's why `"P2 actor registered at pad PAD_2"` appears 2-3 times. The
slot-occupancy check is idempotent — re-entering `create()` for the same
`this` keeps it in slot 1.

### Despawn

`Tick()` exits the `Leaving` state after 12 frames and calls
`DespawnPlayer2()` + `DestroyCamera2()`. Both go through `fpcM_SearchByID` +
`fpcM_Delete`. The patched `camera_delete` callback uses each camera's own
slot to null out, so P2's destruction doesn't touch slot 0.

## 8. Render viewport math

`GetEyeViewport(eye)` in `splitscreen_render.cpp`:

- Eye 0 (left, P1): width lerps from `FB_WIDTH` (full) → `FB_WIDTH/2` (split)
  as `GetTransitionT()` goes 0 → 1.
- Eye 1 (right, P2): width lerps from 0 → `FB_WIDTH/2`, anchored to the right
  edge.

Vertical split (top/bottom) would just swap which axis lerps; same code shape.

The two passes do not share viewport state with each other across frames. The
ortho 2D overlay (HUD, ImGui) is drawn **once** at the end of the function
outside the for-loop, in full-frame coordinates. HUD redesign for per-eye
display is open work (plan §4.3).

## 9. Input routing

```
mDoCPd_c::m_gamePad[0]  ← physical pad-1     ← P1's Link reads via LINK_PAD
mDoCPd_c::m_gamePad[1]  ← physical pad-2     ← P2's Link reads via LINK_PAD
                          (forced-allocated in non-debug builds)
```

The `LINK_PAD` macro in `d_a_alink.cpp` expands to
`dusk_ss::GetPadForActor(this)` when splitscreen is on, and back to the
original `PAD_1` constant when off. The 25 input-read sites in `d_a_alink.cpp`
all flow through it.

Diagnostic / live-test inputs (in `splitscreen_input.cpp`):

| Trigger | Action |
|---|---|
| Pad-2 Start (edge) | Request P2 join |
| Pad-2 Start held 1s | Request P2 leave |
| Pad-1 L + R + D-Up held 0.5s | Warp P2 → P1 |
| **F9** (keyboard) | Request P2 join |
| **F10** | Request P2 leave |
| **F11** | Warp P2 → P1 |
| **F12** | Skip intro / setNextStage("F_SP108", 21, 1, 13) |
| `DUSK_SS_AUTO_JOIN=N` (env) | Auto-join at frame N |
| `DUSK_SS_AUTO_LEAVE=N` | Auto-leave at frame N |
| `DUSK_SS_AUTO_WARP=N` | Auto-warp at frame N |
| `DUSK_SS_SKIP_INTRO=N` | Skip intro at frame N (use small N like 60) |

Keyboard hotkeys are edge-detected via a 512-byte prev-state buffer compared
against `SDL_GetKeyboardState`.

## 10. Why these specific design choices

**One actor / one camera per player, both in pre-existing slot arrays.**
The plan considered keeping P2 state entirely outside the engine struct
(dispatching getter calls through `dusk_ss`). Bumping the arrays to size 2
turned out simpler because the existing `ARRAY_SIZE(mPlayerInfo)` iteration
auto-extends, the layout offset comments are advisory only on PC, and no
external code reaches into these structs by byte offset.

**No offscreen render targets for the two passes.** The plan flagged
offscreen + composite as the fallback if Aurora couldn't tolerate two
scissored passes per swap. It tolerated them fine. Keeping everything in the
same swapchain image avoids extra texture allocation, blit cost, and frame-
interpolation complications.

**Renderer-side `eye` driven, not actor-side.** Each scene-draw pass selects
its camera and player from the eye index; actors don't know which eye is
being rendered for them. This keeps actor code uniform — only `daAlink_c`'s
input reads needed plumbing, not its draw code.

**Slot-occupancy detection over a "spawning P2" flag.** A persistent flag in
`dusk_ss` only stays true during the synchronous `fopAcM_create` call; the
actual `daAlink_c::create()` runs later in the next frame's process
management when the flag is already cleared. Checking
`existing_p1 != nullptr && existing_p1 != this` works regardless of timing
and is idempotent across the multi-phase create cycle.

**Reverse-lookup in `fopCamM_GetParam`.** The cleanest fix would be to
write per-instance into `process->parameters` at create time, but that field
is overwritten by the procname's profile data during `fpcBs_SubCreate`.
The `l_fopCamM_id[]` reverse-lookup is the only path that stays stable
across the create lifecycle.

## 11. Known limitations and follow-ups

These are tracked behaviour caveats, not bugs:

1. **Discord rich presence, achievements, speedrun timer**: still keyed off
   `dComIfGp_getLinkPlayer()` (P1). P2 actions don't grant achievements.
   Acceptable for v1.
2. **HUD layout**: drawn full-screen, not per-eye. Hearts/rupees/ammo overlap
   both viewports. A `dusk_ss::IsActive()`-aware HUD path is open work.
3. **Pause menu**: pauses both players (free for us), but inventory equip
   changes apply globally because the menu still operates on
   `dComIfGp_getLinkPlayer()`. Per-player equip selection is a UX feature
   not done.
4. **Damage routing one-way**: `setDamagePoint` correctly routes damage to
   `OnPlayerDamage(1, ...)` when called on the P2 actor. Healing items
   (pickup hearts) still go to P1 because the pickup code targets
   `getLinkPlayer()`. Drink potions, heart-piece collection: shared / P1.
5. **Cutscene-end warp** detection uses a simple `dComIfGp_event_runCheck()`
   edge. A long event that doesn't fully clear the flag mid-fade may queue a
   spurious warp.

## 12. Validation status

Verified end-to-end in headless mode (Xvfb + Vulkan + NVIDIA driver):

- ✓ Single-player build with flag OFF: byte-identical behaviour to upstream
- ✓ Single-player build with flag ON, no auto-join: clean 60s run to frame 1800
- ✓ Auto-join at frame 900: P2 actor spawns, camera 1 created, two-pass
  renderer engaged (frames 900-1740 logged), no crashes
- ✓ Auto-warp: walkable-cell probe finds same-room ground, P2 snaps to P1
- ✓ Skip-intro: `setNextStage("F_SP108", 21, 1, 13)` jumps past DEMO00
- ✓ Pad-2 hot-unplug → auto drop-out (state-machine path, not crash-tested
  with real device)
- ✓ Clean shutdown: `DespawnPlayer2` + `DestroyCamera2` execute without
  touching P1's slots

Not validated headlessly (require interactive input or save-state past intro):

- Pad-routed input distinguishing P1 vs P2 movement
- Teleport combo gesture timing
- Drop-out hold gesture timing
- Cutscene auto-park during a real in-game cutscene (not the title intro)
- Shared inventory pickup by P2
- Per-player damage / HP / bubble-revive in combat
