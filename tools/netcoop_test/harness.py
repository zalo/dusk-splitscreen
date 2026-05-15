"""Local two-instance test harness for the netcoop module.

Spawns isolated copies of the dusklight binary side-by-side, captures their
stdout/stderr, and exposes a small wait_for_log API so a test script can
assert on the netcoop log lines emitted during discovery, handshake, and
gameplay sync.

Each Instance runs:
  - inside its own xvfb-run -a display (so menu input doesn't cross-leak)
  - with HOME / XDG_DATA_HOME pointed at a private tmpdir (so SDL_GetPrefPath
    returns a unique save / config directory per instance)
  - with stdout+stderr tee'd to a logfile under the same tmpdir
  - in its own process group so a single os.killpg tears down xvfb-run,
    the Xvfb child, and the game binary together

The netcoop module's Init() is gated behind a successful DVD load (it lives
in m_Do_main.cpp main01, which runs only after the prelaunch UI accepts a
valid disc image). So tests that need to observe discovery / handshake must
pass a DVD path via LaunchOptions.dvd. Pass dvd=None to test the prelaunch
UI path explicitly.

Log format from src/dusk/logging.cpp WriteLogLine:
    [LEVEL | dusk] netcoop: ...

Quick start:
    from tools.netcoop_test.harness import LaunchOptions, launch_pair
    a, b = launch_pair(opts=LaunchOptions())
    a.wait_for_log(r"netcoop: peer handshake OK")
    b.wait_for_log(r"netcoop: peer handshake OK")
    a.stop(); a.cleanup(); b.stop(); b.cleanup()
"""

from __future__ import annotations

import os
import re
import signal
import subprocess
import tempfile
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Optional


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BINARY = REPO_ROOT / "build" / "netcoop" / "dusklight"
DEFAULT_DVD    = REPO_ROOT / "rom" / "tp-usa.iso"


@dataclass
class LaunchOptions:
    binary: Path = DEFAULT_BINARY
    backend: str = "vulkan"
    # DVD image to pass via --dvd. The netcoop module's Init() is only called
    # *after* a DVD has been loaded (it lives inside main01, which the
    # prelaunch UI gates on a valid disc), so tests that want to observe
    # discovery / handshake must supply one.
    dvd: Optional[Path] = DEFAULT_DVD
    # When True, set the DUSK_TEST_FAST_BOOT + DUSK_TEST_AUTOSKIP_CUTSCENES
    # env vars on launch. The engine hooks in src/dusk/test_autoboot.cpp,
    # src/d/actor/d_a_title.cpp, src/d/d_s_name.cpp, and src/d/d_event.cpp
    # collectively drive the boot path from title → file-select slot 0 →
    # warp into F_SP108 → autoskip every demo with a canSkip flag → wake
    # up in R_SP01 (Link's house). Each transition is logged so the harness
    # can wait_for_log on a deterministic landmark.
    fast_boot: bool = False
    # Optional fixed main-stick deflection in [-1, 1]. The engine hook in
    # mDoCPd_c::read overrides m_cpadInfo[0]'s stick state every frame, so
    # Link walks in the given direction through the engine's normal
    # movement pipeline. None on either axis leaves real input untouched.
    stick_x: Optional[float] = None
    stick_y: Optional[float] = None
    extra_args: tuple[str, ...] = ()
    # Per-instance cvar overrides. Default disables Discord IPC (which blocks
    # for ~15s waiting on a socket that doesn't exist in CI) and pause-on-
    # focus-loss (xvfb windows aren't really "focused" the way the engine
    # expects).
    cvars: tuple[str, ...] = (
        "game.enableDiscordPresence=false",
        "game.pauseOnFocusLost=false",
    )
    # Total seconds to wait for any single wait_for_log call before giving up.
    default_timeout_s: float = 90.0


class Instance:
    """One running dusklight process under an isolated environment."""

    def __init__(self, name: str, opts: LaunchOptions = LaunchOptions()):
        self.name = name
        self.opts = opts
        self._tmpdir: Optional[tempfile.TemporaryDirectory] = None
        self._proc: Optional[subprocess.Popen] = None
        self._log_path: Optional[Path] = None
        self._log_buf: List[str] = []
        self._log_lock = threading.Lock()
        self._reader_thread: Optional[threading.Thread] = None

    # ------------------------------------------------------------------ lifecycle

    def start(self) -> None:
        if self._proc is not None:
            raise RuntimeError(f"Instance '{self.name}' already started")

        # ignore_cleanup_errors handles the EPERM that surfaces on Linux when
        # the desktop portal (xdg-document-portal) FUSE-mounts a `by-app/`
        # subdir under XDG_CACHE_HOME — we can't unlink it as a normal user,
        # but the kernel will reap it when the mount goes away.
        self._tmpdir = tempfile.TemporaryDirectory(
            prefix=f"netcooptest-{self.name}-",
            ignore_cleanup_errors=True,
        )
        sandbox = Path(self._tmpdir.name)
        (sandbox / "home").mkdir()
        (sandbox / "xdg-data").mkdir()
        (sandbox / "xdg-cache").mkdir()
        (sandbox / "xdg-config").mkdir()
        self._log_path = sandbox / "stdout.log"

        env = os.environ.copy()
        env["HOME"]            = str(sandbox / "home")
        env["XDG_DATA_HOME"]   = str(sandbox / "xdg-data")
        env["XDG_CACHE_HOME"]  = str(sandbox / "xdg-cache")
        env["XDG_CONFIG_HOME"] = str(sandbox / "xdg-config")
        if self.opts.fast_boot:
            env["DUSK_TEST_FAST_BOOT"] = "1"
            env["DUSK_TEST_AUTOSKIP_CUTSCENES"] = "1"
        if self.opts.stick_x is not None:
            env["DUSK_TEST_STICK_X"] = f"{self.opts.stick_x:.4f}"
        if self.opts.stick_y is not None:
            env["DUSK_TEST_STICK_Y"] = f"{self.opts.stick_y:.4f}"
        # Don't inherit a parent DISPLAY: xvfb-run -a will assign its own.
        env.pop("DISPLAY", None)
        env.pop("WAYLAND_DISPLAY", None)
        # Vulkan via llvmpipe (the only ICD likely available under xvfb on a
        # headless box). If the host doesn't have it, the launch will still
        # surface a clean error in the log.
        env.setdefault("MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE", "1")

        binary = self.opts.binary
        if not binary.exists():
            raise FileNotFoundError(
                f"dusklight binary not found at {binary}; "
                f"build with `cmake --build build/netcoop` first"
            )

        cmd: List[str] = [
            "xvfb-run", "-a", "--server-args=-screen 0 1280x720x24",
            str(binary),
            "--backend", self.opts.backend,
        ]
        for cvar in self.opts.cvars:
            cmd += ["--cvar", cvar]
        if self.opts.dvd is not None:
            if not self.opts.dvd.exists():
                raise FileNotFoundError(
                    f"DVD image not found at {self.opts.dvd}; pass dvd=None to "
                    f"skip, or place a valid tp-usa.iso at that path"
                )
            cmd += ["--dvd", str(self.opts.dvd)]
        cmd += list(self.opts.extra_args)

        log_file = self._log_path.open("w", buffering=1)  # line-buffered
        self._log_file_handle = log_file

        # start_new_session=True puts the launched xvfb-run, its Xvfb child,
        # and the dusklight binary in their own process group / session, so
        # stop() can signal the whole tree via os.killpg without a recursive
        # walk.
        self._proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=env,
            text=True,
            bufsize=1,
            start_new_session=True,
        )
        self._reader_thread = threading.Thread(
            target=self._consume_output, daemon=True, name=f"{self.name}-reader"
        )
        self._reader_thread.start()

    def _consume_output(self) -> None:
        assert self._proc is not None and self._proc.stdout is not None
        for raw_line in self._proc.stdout:
            line = raw_line.rstrip("\n")
            with self._log_lock:
                self._log_buf.append(line)
            self._log_file_handle.write(raw_line)

    def stop(self, grace_s: float = 5.0) -> None:
        if self._proc is None:
            return
        pgid = None
        try:
            pgid = os.getpgid(self._proc.pid)
        except (ProcessLookupError, PermissionError):
            pass
        if self._proc.poll() is None:
            try:
                if pgid is not None:
                    os.killpg(pgid, signal.SIGINT)
                else:
                    self._proc.send_signal(signal.SIGINT)
                self._proc.wait(timeout=grace_s)
            except subprocess.TimeoutExpired:
                try:
                    if pgid is not None:
                        os.killpg(pgid, signal.SIGTERM)
                    else:
                        self._proc.terminate()
                    self._proc.wait(timeout=2.0)
                except subprocess.TimeoutExpired:
                    if pgid is not None:
                        try:
                            os.killpg(pgid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                    else:
                        self._proc.kill()
        # Belt-and-suspenders: even after the leader exits, send a final
        # SIGKILL to the group so any Xvfb/xauth siblings that survived the
        # SIGINT are reaped before cleanup.
        if pgid is not None:
            try:
                os.killpg(pgid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass
        if self._reader_thread is not None:
            self._reader_thread.join(timeout=2.0)
        if hasattr(self, "_log_file_handle"):
            self._log_file_handle.close()

    def cleanup(self, keep_logs: bool = False) -> None:
        if self._tmpdir is not None:
            if keep_logs and self._log_path is not None:
                # Preserve only the stdout log for postmortem; let everything
                # else (cache, config, save data) get cleaned up normally.
                kept = Path("/tmp") / f"netcooplog-{self.name}-{self._tmpdir.name.rsplit('-', 1)[-1]}.log"
                try:
                    import shutil
                    shutil.copy(self._log_path, kept)
                    print(f"   (preserved {self.name} log → {kept})")
                except OSError as e:
                    print(f"   (could not preserve {self.name} log: {e})")
            self._tmpdir.cleanup()
            self._tmpdir = None

    # ------------------------------------------------------------------ logs

    def log_path(self) -> Path:
        assert self._log_path is not None
        return self._log_path

    def snapshot_log(self) -> List[str]:
        with self._log_lock:
            return list(self._log_buf)

    def find(self, pattern: str | re.Pattern) -> Optional[re.Match]:
        """Search the buffered log for the first line that matches `pattern`."""
        rx = pattern if isinstance(pattern, re.Pattern) else re.compile(pattern)
        with self._log_lock:
            for line in self._log_buf:
                m = rx.search(line)
                if m:
                    return m
        return None

    def wait_for_log(
        self,
        pattern: str | re.Pattern,
        timeout_s: Optional[float] = None,
        poll_interval_s: float = 0.1,
    ) -> re.Match:
        """Block until `pattern` shows up in the log, or raise TimeoutError."""
        rx = pattern if isinstance(pattern, re.Pattern) else re.compile(pattern)
        deadline = time.monotonic() + (timeout_s or self.opts.default_timeout_s)
        while time.monotonic() < deadline:
            m = self.find(rx)
            if m:
                return m
            if self._proc is not None and self._proc.poll() is not None:
                raise RuntimeError(
                    f"Instance '{self.name}' exited (rc={self._proc.returncode}) "
                    f"before pattern matched: {rx.pattern}"
                )
            time.sleep(poll_interval_s)
        raise TimeoutError(
            f"Instance '{self.name}' did not log pattern within "
            f"{timeout_s or self.opts.default_timeout_s}s: {rx.pattern}"
        )

    def tail(self, n: int = 40) -> List[str]:
        with self._log_lock:
            return list(self._log_buf[-n:])

    # ------------------------------------------------------------ scripted boots

    def boot_to_ordon(
        self, timeout_s: float = 600.0,
    ) -> re.Match:
        """Drive this instance from launch to "Link is interactive in Ordon."

        Requires the instance to have been started with LaunchOptions(fast_boot=
        True) — that sets the DUSK_TEST_FAST_BOOT and DUSK_TEST_AUTOSKIP_CUTSCENES
        env vars that the engine hooks read.

        Stage timeline observed in TP-USA with autoskip:
            F_SP102 (boot/title/file-select scaffolding) → events-idle
            F_SP108 (Faron Spring intro w/ Rusl)         → events-idle (skipped)
            F_SP103 (Ordon Village exterior)             → events-idle  ← target

        The autoskip blows through the R_SP01 (Link's bedroom) wake-up demo
        without R_SP01 ever being a distinct stage load — the engine warps
        directly to F_SP103 once the demo's skip handler fires. So "Link is
        running around Ordon Village" means F_SP103 events-idle in practice;
        Link's house is reachable from there if a test wants to drive into it.

        Returns the regex match for the stage-change log line that landed us
        in F_SP103. After this returns Link is interactive — no main demo is
        running, control is the player's.

        The 10-minute default timeout is generous; a clean run lands in
        F_SP103 events-idle within 60-90s, dominated by cold disc reads.
        """
        if not self.opts.fast_boot:
            raise RuntimeError(
                f"Instance '{self.name}' must be launched with "
                f"LaunchOptions(fast_boot=True) for boot_to_ordon"
            )

        deadline = time.monotonic() + timeout_s

        def remaining() -> float:
            r = deadline - time.monotonic()
            if r <= 0:
                raise TimeoutError(
                    f"boot_to_ordon('{self.name}') exceeded {timeout_s}s"
                )
            return r

        landed = self.wait_for_log(
            r"test: stage-change to=F_SP103 ",
            timeout_s=remaining(),
        )
        # The stage-change fires when the new stage is requested, but a
        # waking-up demo runs briefly before player control unlocks. Wait
        # for the matching events-idle so the caller knows Link is truly
        # interactive.
        self.wait_for_log(
            r"test: events-idle stage=F_SP103",
            timeout_s=min(remaining(), 60.0),
        )
        return landed


# Convenience re-exports / helpers --------------------------------------------

def launch_pair(
    names: Iterable[str] = ("inst1", "inst2"),
    opts: LaunchOptions = LaunchOptions(),
    stagger_s: float = 1.0,
) -> List[Instance]:
    """Launch two (or more) instances back-to-back with a small stagger so the
    first one wins the lower port. Caller is responsible for stop()/cleanup()."""
    name_list = list(names)
    insts: List[Instance] = []
    try:
        for i, name in enumerate(name_list):
            inst = Instance(name, opts)
            inst.start()
            insts.append(inst)
            if i + 1 < len(name_list):
                time.sleep(stagger_s)
        return insts
    except Exception:
        for inst in insts:
            inst.stop()
            inst.cleanup()
        raise
