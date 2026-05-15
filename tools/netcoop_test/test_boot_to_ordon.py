#!/usr/bin/env python3
"""Exercise the boot_to_ordon harness function.

Launches a single dusklight instance with fast_boot=True, expects the engine's
test_autoboot hooks to drive: title → file-select slot 0 (auto-created with
default names) → F_SP108 intro → autoskip every cutscene → F_SP103 (Ordon
Village exterior) → events-idle (Link interactive in Ordon).

Then optionally repeats with TWO instances launched simultaneously to confirm
both can reach the interactive state and complete their netcoop handshake.

Usage:
    python3 tools/netcoop_test/test_boot_to_ordon.py            # single
    python3 tools/netcoop_test/test_boot_to_ordon.py --both     # both
    python3 tools/netcoop_test/test_boot_to_ordon.py --timeout 900

Exit code: 0 on pass, 1 on failure.

--both runs the two-instance flow sequentially (inst1 fully settled in
Ordon before inst2 launches) and then asserts the ghost actors are
registered AND streaming live data on each side.
"""

from __future__ import annotations

import argparse
import math
import random
import re
import sys
import time
from dataclasses import replace
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from netcoop_test.harness import (  # noqa: E402
    DEFAULT_BINARY,
    DEFAULT_DVD,
    Instance,
    LaunchOptions,
    launch_pair,
)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    p.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    p.add_argument("--dvd", type=Path, default=DEFAULT_DVD)
    p.add_argument("--backend", default="vulkan")
    p.add_argument("--timeout", type=float, default=600.0,
                   help="boot_to_ordon timeout per instance, seconds (default: 600)")
    p.add_argument("--both", action="store_true",
                   help="Also run a two-instance simultaneous boot to verify "
                        "netcoop + autoboot interact cleanly")
    return p.parse_args()


def dump_tail(inst: Instance, n: int = 50) -> None:
    print(f"\n--- {inst.name} log tail (last {n} lines) ---")
    for line in inst.tail(n):
        print(line)


def run_single(opts: LaunchOptions, timeout: float) -> bool:
    print(f"== boot_to_ordon (single instance) ==")
    print(f"   binary  : {opts.binary}")
    print(f"   dvd     : {opts.dvd}")
    print(f"   backend : {opts.backend}")
    print(f"   timeout : {timeout}s")

    inst = Instance("solo", opts)
    inst.start()
    t0 = time.monotonic()
    try:
        match = inst.boot_to_ordon(timeout_s=timeout)
        elapsed = time.monotonic() - t0
        print(f"   [solo] OK — Link interactive in Ordon Village in {elapsed:.1f}s")
        print(f"          landmark: {match.group(0).strip()}")
        return True
    except (TimeoutError, RuntimeError) as e:
        print(f"   [solo] FAIL — {e}")
        dump_tail(inst)
        return False
    finally:
        inst.stop()
        inst.cleanup()


def run_pair(opts: LaunchOptions, timeout: float) -> bool:
    """Bring up two instances sequentially and verify ghost actor sync.

    Sequence:
      1. Launch inst1 alone, boot_to_ordon → F_SP103 events-idle.
         inst1 is the lower port (47100) so it'll act as the server when
         a peer arrives, broadcasting WorldLocation = F_SP103 once paired.
      2. Launch inst2, boot_to_ordon → F_SP103 events-idle. During its boot,
         netcoop discovery finds inst1, handshake completes, the host's
         WorldLocation lands inst2 in F_SP103.
      3. Wait for both sides to log "netcoop: ghost actor registered" with a
         non-null pointer — proves the peer actor exists in each game's
         scene graph.
      4. Wait for the netcoop "peer @ (x, y, z) yaw=... anm=... frame=..."
         periodic dump on each side, twice, and confirm the animation frame
         number advances between samples — proves the wire is delivering
         live data, not a stale single snapshot. Idle Link plays a breathing
         animation so animFrame ticks even without input.

    Going sequential dodges the race where two concurrent instances both
    hit F_SP102 → F_SP108 stage transitions at the same time as ghost-spawn
    requests; with inst1 already settled in F_SP103, inst2 transitions
    happen against a stable peer.
    """
    print(f"\n== boot_to_ordon (sequential two-instance + ghost sync) ==")

    # Pick a random run-direction per instance. The engine hook in
    # mDoCPd_c::read sees these as a fixed stick deflection and walks
    # Link through the normal movement system once F_SP103 is loaded.
    rng = random.Random()
    angle1 = rng.uniform(0.0, 2 * math.pi)
    angle2 = rng.uniform(0.0, 2 * math.pi)
    opts1 = replace(opts, stick_x=math.sin(angle1), stick_y=math.cos(angle1))
    opts2 = replace(opts, stick_x=math.sin(angle2), stick_y=math.cos(angle2))
    print(f"   inst1 will run heading {math.degrees(angle1):6.1f}° "
          f"(stick {opts1.stick_x:+.2f}, {opts1.stick_y:+.2f})")
    print(f"   inst2 will run heading {math.degrees(angle2):6.1f}° "
          f"(stick {opts2.stick_x:+.2f}, {opts2.stick_y:+.2f})")

    inst1 = Instance("inst1", opts1)
    inst2 = Instance("inst2", opts2)
    failures: list[str] = []
    try:
        # --- Phase 1: bring inst1 up alone. -------------------------------
        print("   [phase 1] booting inst1 (will own port 47100)...")
        t0 = time.monotonic()
        inst1.start()
        try:
            inst1.boot_to_ordon(timeout_s=timeout)
        except (TimeoutError, RuntimeError) as e:
            print(f"   [inst1] FAIL — {e}")
            dump_tail(inst1, n=60)
            return False
        print(f"   [inst1] OK — interactive in Ordon at {time.monotonic()-t0:.1f}s")

        # --- Phase 2: bring inst2 up, expecting it to dial inst1. ---------
        print("   [phase 2] booting inst2 (will dial inst1)...")
        t1 = time.monotonic()
        inst2.start()
        try:
            inst2.boot_to_ordon(timeout_s=timeout)
        except (TimeoutError, RuntimeError) as e:
            print(f"   [inst2] FAIL — {e}")
            dump_tail(inst2, n=80)
            dump_tail(inst1, n=20)
            return False
        print(f"   [inst2] OK — interactive in Ordon at {time.monotonic()-t1:.1f}s")

        # --- Phase 3: handshake landed on both sides ----------------------
        for inst in (inst1, inst2):
            if not inst.find(r"netcoop: peer handshake OK"):
                failures.append(f"{inst.name} never completed handshake")
            else:
                print(f"   [{inst.name}] handshake ✓")

        # --- Phase 4: ghost actor spawned + registered (non-null) ---------
        ghost_rx = re.compile(r"netcoop: ghost actor registered \((0x[0-9a-f]+)\)")
        for inst in (inst1, inst2):
            try:
                m = inst.wait_for_log(ghost_rx, timeout_s=30.0)
                ptr = m.group(1)
                if ptr == "0x0":
                    failures.append(f"{inst.name}'s first ghost register was null")
                else:
                    print(f"   [{inst.name}] ghost actor @ {ptr} ✓")
            except (TimeoutError, RuntimeError) as e:
                failures.append(f"{inst.name} no ghost actor registered: {e}")

        # --- Phase 5: each side's Link runs and the peer reflects it ------
        # Each instance has a fixed random stick deflection pointing in a
        # different direction; the engine hook in mDoCPd_c::read applies it
        # every frame once stage F_SP103 is loaded. The peer @ log on the
        # opposite side should report a position significantly different
        # from its spawn point — proving (a) Link is actually moving in
        # response to the synthetic input on its own side, and (b) that
        # motion propagates through the netcoop wire to the ghost on the
        # peer side.
        spawn_rx = re.compile(
            r"test: local-spawn @ \((-?[\d.]+), (-?[\d.]+), (-?[\d.]+)\)"
        )
        spawn = {}
        for inst in (inst1, inst2):
            try:
                m = inst.wait_for_log(spawn_rx, timeout_s=30.0)
                spawn[inst.name] = (float(m.group(1)), float(m.group(3)))
                print(f"   [{inst.name}] local spawn @ "
                      f"x={spawn[inst.name][0]:.0f}, z={spawn[inst.name][1]:.0f}")
            except (TimeoutError, RuntimeError) as e:
                failures.append(f"{inst.name} no local-spawn baseline: {e}")
                spawn[inst.name] = (0.0, 0.0)

        # capture log lengths now so we only consider peer-position lines
        # that appear after both peers are settled
        log_start_idx = {inst.name: len(inst.snapshot_log()) for inst in (inst1, inst2)}

        peer_rx = re.compile(
            r"netcoop: peer @ \((-?[\d.]+), (-?[\d.]+), (-?[\d.]+)\) "
            r"yaw=(-?[\d.]+) anm=(\d+) frame=([\d.]+) sf=(\d+)"
        )
        # Each periodic dump is ~6s apart, so 25s reliably yields 3-4 samples.
        for inst in (inst1, inst2):
            t_start = time.monotonic()
            samples: list[tuple[float, float, int, int]] = []
            while time.monotonic() - t_start < 25.0 and len(samples) < 3:
                buf = inst.snapshot_log()
                samples = []
                for line in buf[log_start_idx[inst.name]:]:
                    m = peer_rx.search(line)
                    if m:
                        x = float(m.group(1)); z = float(m.group(3))
                        anm = int(m.group(5)); sf = int(m.group(7))
                        samples.append((x, z, anm, sf))
                    if len(samples) >= 3:
                        break
                if len(samples) < 3:
                    time.sleep(0.5)

            if len(samples) < 2:
                failures.append(
                    f"{inst.name} got only {len(samples)} post-settle peer "
                    f"samples in 25s — sync wire appears silent"
                )
                continue

            # The peer's serverFrame counter is incremented every time *their*
            # local Link runs daAlink::execute(). If we see two consecutive
            # samples with the same serverFrame, the peer's send path is
            # frozen (or our receive path is wedged). Position/anim drift is
            # a secondary signal — useful in logs, not load-bearing for the
            # assertion since an idle Link with no input can legitimately
            # hold the same pose for seconds.
            sf_delta = samples[-1][3] - samples[0][3]
            x_last   = samples[-1][0]
            z_last   = samples[-1][1]

            # The peer's spawn position (what their local Link started at
            # when F_SP103 first loaded). Each side moves from there; we
            # check that the position we *receive* from the peer differs
            # significantly from where they started.
            peer = inst1 if inst is inst2 else inst2
            sx, sz = spawn.get(peer.name, (0.0, 0.0))
            drift_from_spawn = math.hypot(x_last - sx, z_last - sz)

            # Two assertions:
            #  (a) sf advances → peer's send loop is alive
            #  (b) the peer's latest position differs by more than 50 units
            #      from where they spawned → motion-sync end-to-end. A walk
            #      cycle in TP covers ~500 units/s; in ~25s of running we
            #      expect thousands of units of travel, but Link will hit
            #      Ordon's geometry (fences, river, houses) quickly, so we
            #      keep the floor at 50 — enough to rule out a stuck peer
            #      while tolerating any obstacle Link runs into.
            sf_tag    = "✓" if sf_delta > 0          else "✗"
            drift_tag = "✓" if drift_from_spawn > 50 else "✗"
            print(f"   [{inst.name}] peer samples (n={len(samples)}): "
                  f"sf Δ={sf_delta} {sf_tag}  "
                  f"peer pos ({x_last:.0f},{z_last:.0f}) "
                  f"is {drift_from_spawn:.0f}u from their spawn "
                  f"({sx:.0f},{sz:.0f}) {drift_tag}")
            if sf_tag == "✗":
                failures.append(
                    f"{inst.name} saw {len(samples)} peer snapshots but "
                    f"serverFrame didn't advance — peer's send loop is frozen"
                )
            if drift_tag == "✗":
                failures.append(
                    f"{inst.name}: peer reached only {drift_from_spawn:.0f}u "
                    f"from spawn after stick injection — Link not moving, "
                    f"or position not propagating through the wire"
                )

        for f in failures:
            print(f"   FAIL — {f}")
        if failures:
            for inst in (inst1, inst2):
                dump_tail(inst)
            return False
        print("   ghost actors synchronizing live on both instances ✓")
        return True
    finally:
        keep_logs = bool(failures)
        for inst in (inst1, inst2):
            inst.stop()
            inst.cleanup(keep_logs=keep_logs)


def main() -> int:
    args = parse_args()

    opts = LaunchOptions(
        binary=args.binary,
        backend=args.backend,
        dvd=args.dvd,
        fast_boot=True,
        default_timeout_s=args.timeout,
    )

    single_ok = run_single(opts, args.timeout)
    if not single_ok:
        return 1

    if args.both:
        pair_ok = run_pair(opts, args.timeout)
        if not pair_ok:
            return 1

    print("\n== PASS ==")
    return 0


if __name__ == "__main__":
    sys.exit(main())
