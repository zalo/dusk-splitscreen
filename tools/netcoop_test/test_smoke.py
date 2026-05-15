#!/usr/bin/env python3
"""Smoke test for netcoop discovery + handshake + initial-snapshot exchange.

Spawns two isolated dusklight processes and asserts the netcoop log lines
emitted before the file-select menu has been navigated past:

  inst1 (server role):
    [INFO | dusk] netcoop: Init (uuid=...)
    [INFO | dusk] netcoop: listening on 127.0.0.1:47100 ...
    [INFO | dusk] netcoop: peer handshake OK — uuid=... save=0 role=server
    [INFO | dusk] netcoop: toast — Player joined (port 47101)
    [INFO | dusk] netcoop: queued initial SaveSnapshot

  inst2 (client role):
    [INFO | dusk] netcoop: Init (uuid=...)
    [INFO | dusk] netcoop: listening on 127.0.0.1:47101 ...
    [INFO | dusk] netcoop: dialed peer on 127.0.0.1:47100
    [INFO | dusk] netcoop: peer handshake OK — uuid=... save=0 role=client
    [INFO | dusk] netcoop: queued initial SaveSnapshot

Usage:
    python3 tools/netcoop_test/test_smoke.py
    python3 tools/netcoop_test/test_smoke.py --binary path/to/dusklight
    python3 tools/netcoop_test/test_smoke.py --backend null   # less noise

Exit code: 0 on pass, 1 on any failed assertion or timeout.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

# Allow running as a script (without installing the package) by adding the
# parent directory of this file to sys.path.
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
    p.add_argument("--binary", type=Path, default=DEFAULT_BINARY,
                   help=f"Path to dusklight binary (default: {DEFAULT_BINARY})")
    p.add_argument("--dvd", type=Path, default=DEFAULT_DVD,
                   help=f"DVD image to pass via --dvd (default: {DEFAULT_DVD})")
    p.add_argument("--backend", default="vulkan",
                   help="Aurora graphics backend to request (default: vulkan)")
    p.add_argument("--timeout", type=float, default=90.0,
                   help="Per-assertion timeout in seconds (default: 90)")
    return p.parse_args()


def run(args: argparse.Namespace) -> int:
    opts = LaunchOptions(
        binary=args.binary,
        backend=args.backend,
        dvd=args.dvd,
        default_timeout_s=args.timeout,
    )

    print(f"== netcoop smoke test ==")
    print(f"   binary  : {opts.binary}")
    print(f"   dvd     : {opts.dvd}")
    print(f"   backend : {opts.backend}")
    print(f"   timeout : {opts.default_timeout_s}s per assertion")

    insts: list[Instance] = []
    failures: list[str] = []

    def assert_log(inst: Instance, pattern: str, label: str) -> None:
        try:
            m = inst.wait_for_log(pattern)
            print(f"   [{inst.name}] OK  — {label} :: {m.group(0)[:120]}")
        except (TimeoutError, RuntimeError) as e:
            failures.append(f"[{inst.name}] {label} :: {e}")
            print(f"   [{inst.name}] FAIL — {label} :: {e}")

    try:
        insts = launch_pair(("inst1", "inst2"), opts=opts, stagger_s=1.5)
        a, b = insts

        # 1. Both instances initialize the module.
        assert_log(a, r"netcoop: Init \(uuid=([0-9a-f]{16})\)", "Init")
        assert_log(b, r"netcoop: Init \(uuid=([0-9a-f]{16})\)", "Init")

        # 2. Both instances bind to a port in the 47100–47109 window.
        assert_log(a, r"netcoop: listening on 127\.0\.0\.1:(4710\d) \(uuid=", "listening")
        assert_log(b, r"netcoop: listening on 127\.0\.0\.1:(4710\d) \(uuid=", "listening")

        m_a_port = a.find(r"listening on 127\.0\.0\.1:(4710\d)")
        m_b_port = b.find(r"listening on 127\.0\.0\.1:(4710\d)")
        if m_a_port and m_b_port:
            pa, pb = m_a_port.group(1), m_b_port.group(1)
            if pa == pb:
                failures.append(f"port collision: both bound to {pa}")
            else:
                print(f"   ports  inst1={pa} inst2={pb} — distinct ✓")

        # 3. Exactly one side should report dialing the peer (client role).
        #    The other side accepted the inbound connection (server role).
        assert_log(b, r"netcoop: dialed peer on 127\.0\.0\.1:4710\d", "dialed peer")

        # 4. Both report a successful handshake. The uuid each side reports is
        #    its *peer's* uuid; we just check the line, not the value.
        assert_log(
            a, r"netcoop: peer handshake OK — uuid=[0-9a-f]{16} save=\d+ role=server",
            "handshake (server)",
        )
        assert_log(
            b, r"netcoop: peer handshake OK — uuid=[0-9a-f]{16} save=\d+ role=client",
            "handshake (client)",
        )

        # 5. Both queue an initial save snapshot to ship to the peer.
        assert_log(a, r"netcoop: queued initial SaveSnapshot", "snapshot queued")
        assert_log(b, r"netcoop: queued initial SaveSnapshot", "snapshot queued")

        # 6. The server-side instance should emit the "Player joined" toast
        #    (the client side never sets s_wasConnected=true *before* state
        #    transitions; both should see it now in the modern code path).
        assert_log(a, r"netcoop: toast — Player joined \(port 4710\d\)", "player-join toast")

        # On failure, dump the tail of each log so the user has something to
        # grep without having to mount the tmpdirs.
        if failures:
            for inst in insts:
                print(f"\n--- {inst.name} log tail (last 30 lines) ---")
                for line in inst.tail(30):
                    print(line)
            return 1

        print("\n== PASS ==")
        return 0

    finally:
        # Give each instance ~0.5s to finalize logs before we tear them down.
        time.sleep(0.5)
        for inst in insts:
            inst.stop()
            inst.cleanup()


if __name__ == "__main__":
    sys.exit(run(parse_args()))
