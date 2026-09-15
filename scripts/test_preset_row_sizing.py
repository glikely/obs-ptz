#!/usr/bin/env python3
"""Checks that PTZPresetListDelegate's row height
(src/ptz-controls.cpp, PTZPresetListDelegate::refreshTheme()/
rowHeightFor()/densityMetricsFor()) matches the real Sources dock's
row height, that ptzToolbar/presetToolbar's height
(PTZControls::showEvent()'s pad) matches the Sources dock's own
toolbar height, and that the preset list's recall icon
(PTZPresetListDelegate::iconSize) matches the Sources dock's own
vis/lock checkbox-icon size, across every Settings > Appearance >
Density preset and every FontScale value reachable through the
real Settings dialog.

Drives the "appearance_row_sizing" test in the UI test harness
(tests/ui-harness/ - see its README.md for the harness itself, and
tests/ui-harness/appearance-row-sizing-test.cpp for what this
particular test does and why it has to run this way) via obs-websocket
- see obs_ws_client.py (imported directly, not shelled out to), and
tests/ui-harness/ui-test-harness.cpp's PTZUITestHarness for the
obs-ptz "vendor" it's calling.

Cross-platform (macOS/Linux/Windows) - the harness and obs-websocket
transport already were; this script's own job is just knowing each
platform's OBS binary location, config directory, and how to ask a
running OBS to quit cleanly. Requires:
  - A real OBS install whose obs-ptz.plugin/.so/.dll is this checkout's
    dev build, built with -DENABLE_UI_TESTS=ON - rebuild that target
    before running this script if src/ptz-controls.cpp or
    tests/ui-harness/* changed.
  - obs-websocket (bundled with OBS) enabled, on the port/password in
    its own config.json (read automatically below - this only works
    because that file is on the same machine this script runs on,
    same assumption everything else here makes).
  - python3, stdlib only - no pip installs, on any platform.
  - The current scene collection to have at least one source, so the
    Sources dock has a row to measure.

Launches OBS exactly once, then sweeps every (Density, FontScale)
combination live, no relaunch per combination - each one pops the real
Settings dialog open and closed, so don't drive the mouse/keyboard
while this runs.

Restores the original Density/FontScale on exit (including on
failure/Ctrl-C - not on a hard kill of this script, e.g. SIGKILL/
Task Manager "End task", which no process can intercept on any
platform).

Usage: python3 scripts/test_preset_row_sizing.py
"""
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))
import obs_ws_client  # noqa: E402

DENSITIES = [-2, -3, -4, -5]
# Settings > Appearance's FontScale slider (AbsoluteSlider
# "appearanceFontScale" in frontend/forms/OBSBasicSettings.ui) only
# covers 8-12 - unlike a direct config_set_int(), which OBS's rendering
# doesn't validate, so a value outside that range is a real,
# correctly-handled config state but not one drivable through the real
# dialog. Requesting one here would silently clamp to 12; if that's
# already the current value, OBS's own dirty-tracking
# (OBSBasicSettings::AppearanceChanged(), gated by "if (!loading)")
# sees no change and skips SaveAppearanceSettings() entirely. So this
# script only sweeps what a user could actually reach.
FONT_SCALES = [8, 9, 10, 11, 12]

# Not DENSITIES[0]/FONT_SCALES[0], and not each other: OBS's Settings
# dialog only saves on an actual value change (see FONT_SCALES's own
# comment above), so if the pre-launch baseline or a warm-up value
# happened to match the next request, that request's
# SaveAppearanceSettings() would be silently skipped and the
# measurement would reflect PTZPresetListDelegate's stale
# construction-time state instead (read before OBS's own theme init
# has necessarily finished applying FontScale to the view's font -
# empirically this can take a couple of Settings-dialog cycles right
# after a fresh launch, not fully deterministic). Distinct values
# throughout guarantee every request, including the first real one, is
# a genuine change the dialog will actually save.
PRELAUNCH_DENSITY, PRELAUNCH_FONTSCALE = -4, 9
WARMUP_REQUESTS = [(-3, 11), (-5, 9)]


class TestError(Exception):
    pass


def obs_config_dir() -> Path:
    system = platform.system()
    if system == "Darwin":
        return Path.home() / "Library" / "Application Support" / "obs-studio"
    if system == "Windows":
        appdata = os.environ.get("APPDATA")
        if not appdata:
            raise TestError("%APPDATA% is not set")
        return Path(appdata) / "obs-studio"
    # Linux and other XDG-following systems
    xdg_config_home = os.environ.get("XDG_CONFIG_HOME")
    base = Path(xdg_config_home) if xdg_config_home else Path.home() / ".config"
    return base / "obs-studio"


def find_obs_binary() -> Path:
    system = platform.system()
    if system == "Darwin":
        path = Path("/Applications/OBS.app/Contents/MacOS/OBS")
    elif system == "Windows":
        program_files = os.environ.get("ProgramFiles", r"C:\Program Files")
        path = Path(program_files) / "obs-studio" / "bin" / "64bit" / "obs64.exe"
    else:
        which = shutil.which("obs")
        if not which:
            raise TestError("'obs' not found on PATH")
        return Path(which)
    if not path.exists():
        raise TestError(f"{path} not found")
    return path


def obs_process_name() -> str:
    return {"Darwin": "OBS", "Windows": "obs64.exe"}.get(platform.system(), "obs")


def is_obs_running() -> bool:
    name = obs_process_name()
    if platform.system() == "Windows":
        out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}"], capture_output=True, text=True)
        return name.lower() in out.stdout.lower()
    return subprocess.run(["pgrep", "-x", name], stdout=subprocess.DEVNULL).returncode == 0


def quit_obs(timeout=10):
    """Asks a running OBS to quit cleanly, then waits for it to exit.

    OBS installs real signal handlers for SIGTERM (frontend/obs-main.cpp,
    frontend/OBSApp.cpp) that drive the same clean Qt shutdown path a
    menu-quit does - true on both macOS and Linux, since that's POSIX
    sigaction() code shared by both. So a plain SIGTERM (`pkill` without
    -9, which defaults to SIGTERM) is a graceful quit on either, no
    macOS-specific "ask the app nicely" mechanism needed. Windows has no
    signal-based equivalent; `taskkill` without /F sends WM_CLOSE, which
    Qt's normal close-event handling picks up the same way a window's
    own close button would.
    """
    name = obs_process_name()
    if platform.system() == "Windows":
        subprocess.run(["taskkill", "/IM", name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        subprocess.run(["pkill", "-TERM", "-x", name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    deadline = time.time() + timeout
    while time.time() < deadline:
        if not is_obs_running():
            return
        time.sleep(0.5)

    # Force-kill fallback if it didn't quit in time.
    if platform.system() == "Windows":
        subprocess.run(["taskkill", "/F", "/IM", name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        subprocess.run(["pkill", "-9", "-x", name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)


def read_ini_value(path: Path, key: str):
    text = path.read_text()
    m = re.search(rf"^{re.escape(key)}=(.*)$", text, re.MULTILINE)
    return m.group(1) if m else None


def write_ini_value(path: Path, key: str, value):
    # A targeted single-line regex substitution, not a full INI
    # parse/rewrite, so every other line (comments, formatting, key
    # order, other sections) is left exactly as OBS wrote it.
    text = path.read_text()
    new_text, n = re.subn(rf"^{re.escape(key)}=.*$", f"{key}={value}", text, count=1, flags=re.MULTILINE)
    if n == 0:
        raise TestError(f"{key}= not found in {path}")
    path.write_text(new_text)


def latest_log_file(log_dir: Path):
    logs = list(log_dir.glob("*.txt"))
    return max(logs, key=lambda p: p.stat().st_mtime) if logs else None


class LogTail:
    """Tracks how much of a growing log file has already been consumed,
    same idea as the previous bash version's `prev_lines` line-count
    tracking - so "wait for a fresh X" only ever looks at genuinely new
    content, not a line from an earlier, already-handled measurement."""

    def __init__(self, path: Path):
        self.path = path
        self.offset = 0

    def new_content(self) -> str:
        with open(self.path, "r", errors="replace") as f:
            f.seek(self.offset)
            data = f.read()
            self.offset = f.tell()
        return data

    def wait_for(self, pattern: str, timeout: float, poll: float = 0.2) -> str:
        """Waits for `pattern` to appear in content that arrives after
        this call starts, returning all new content accumulated by
        then (so a caller can pull other lines - e.g. the matching
        "preset" line alongside the "sources" line - out of the same
        window). Raises TestError on timeout."""
        deadline = time.time() + timeout
        content = ""
        while time.time() < deadline:
            content += self.new_content()
            if re.search(pattern, content):
                return content
            time.sleep(poll)
        raise TestError(f"timed out waiting for {pattern!r} in {self.path}")


# obs-websocket's own RequestStatus::NotReady (requesthandler/types/
# RequestStatus.h) - it gates *all* incoming requests, not just ours,
# behind an internal "OBS itself has finished starting up" flag
# (WebSocketServer_Protocol.cpp's _obsReady) that's a separate,
# later milestone than our own harness reporting itself active in the
# log: the harness can be up and listening for obs-websocket vendor
# registration well before OBS considers itself fully started.
REQUEST_STATUS_NOT_READY = 207


def run_ui_test(ws_port, ws_password, density, fontscale, timeout=30):
    """Retries on RequestStatus::NotReady rather than a fixed sleep
    before the first request: how long OBS takes to consider itself
    ready varies by machine and by how much else the loaded scene
    collection makes it do at startup."""
    deadline = time.time() + timeout
    while True:
        result = obs_ws_client.call_vendor_request(
            "127.0.0.1",
            ws_port,
            ws_password,
            "obs-ptz",
            "ui_test_run",
            {"cmd": "appearance_row_sizing", "density": str(density), "fontscale": str(fontscale)},
            timeout=10,
        )
        if result.get("requestStatus", {}).get("code") != REQUEST_STATUS_NOT_READY:
            return result
        if time.time() >= deadline:
            return result
        time.sleep(0.5)


def parse_row_heights(content: str):
    preset = re.findall(r"\[ptz-ui-test\] preset rowHeight=(-?\d+)", content)
    sources = re.findall(r"\[ptz-ui-test\] sources rowHeight=(-?\d+)", content)
    if not preset or not sources:
        return None, None
    return int(preset[-1]), int(sources[-1])


def parse_toolbar_heights(content: str):
    def last(label):
        m = re.findall(rf"\[ptz-ui-test\] {re.escape(label)} toolbarHeight=(-?\d+)", content)
        return int(m[-1]) if m else None

    return last("ptzToolbar"), last("presetToolbar"), last("sourcesToolbar")


def parse_icon_sizes(content: str):
    preset = re.findall(r"\[ptz-ui-test\] preset recallIconSize=(-?\d+)", content)
    sources = re.findall(r"\[ptz-ui-test\] sources .*checkboxIconSize=(-?\d+)", content)
    if not preset or not sources:
        return None, None
    return int(preset[-1]), int(sources[-1])


def main():
    config_dir = obs_config_dir()
    cfg = config_dir / "user.ini"
    ws_cfg = config_dir / "plugin_config" / "obs-websocket" / "config.json"
    log_dir = config_dir / "logs"

    for f in (cfg, ws_cfg):
        if not f.exists():
            print(f"error: {f} not found", file=sys.stderr)
            return 1
    try:
        obs_bin = find_obs_binary()
    except TestError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    ws_config = json.loads(ws_cfg.read_text())
    if not ws_config.get("server_enabled"):
        print(
            f"error: obs-websocket's server_enabled is false in {ws_cfg} - "
            "enable it in OBS's Tools > obs-websocket Settings",
            file=sys.stderr,
        )
        return 1
    ws_port = ws_config["server_port"]
    ws_password = ws_config["server_password"]

    orig_density = read_ini_value(cfg, "Density")
    orig_fontscale = read_ini_value(cfg, "FontScale")

    total = 0
    failures = 0

    try:
        quit_obs()

        write_ini_value(cfg, "Density", PRELAUNCH_DENSITY)
        write_ini_value(cfg, "FontScale", PRELAUNCH_FONTSCALE)

        env = {**os.environ, "PTZ_UI_TEST_HARNESS": "1"}
        subprocess.Popen(
            [str(obs_bin)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True
        )

        before = latest_log_file(log_dir)
        deadline = time.time() + 15
        logfile = before
        while time.time() < deadline:
            logfile = latest_log_file(log_dir)
            if logfile is not None and logfile != before:
                break
            time.sleep(1)
        if logfile is None:
            print("error: OBS produced no log file", file=sys.stderr)
            return 1

        tail = LogTail(logfile)
        try:
            tail.wait_for(r"\[ptz-ui-test\] harness active", timeout=15)
        except TestError:
            print(
                f"error: harness didn't report active in {logfile} - was obs-ptz built with -DENABLE_UI_TESTS=ON?",
                file=sys.stderr,
            )
            return 1

        for density, fontscale in WARMUP_REQUESTS:
            run_ui_test(ws_port, ws_password, density, fontscale)
            tail.wait_for(r"\[ptz-ui-test\] sources .*checkboxIconSize=", timeout=30)

        for density in DENSITIES:
            for fontscale in FONT_SCALES:
                total += 1
                label = f"Density={density} FontScale={fontscale}"

                try:
                    result = run_ui_test(ws_port, ws_password, density, fontscale)
                except (obs_ws_client.ObsWebSocketError, OSError) as e:
                    print(f"FAIL {label}: obs-websocket CallVendorRequest itself failed ({e})")
                    failures += 1
                    continue

                if not result.get("requestStatus", {}).get("result"):
                    print(f"FAIL {label}: obs-websocket rejected the request: {result}")
                    failures += 1
                    continue

                # The test pops a real modal dialog and clicks through
                # it - give it a generous margin before calling a
                # combination stuck.
                try:
                    content = tail.wait_for(r"\[ptz-ui-test\] sources .*checkboxIconSize=", timeout=30)
                except TestError:
                    print(f"FAIL {label}: no fresh measurement appeared in {logfile}")
                    failures += 1
                    continue

                preset_height, sources_height = parse_row_heights(content)
                ptz_toolbar, preset_toolbar, sources_toolbar = parse_toolbar_heights(content)
                preset_icon, sources_icon = parse_icon_sizes(content)

                if preset_height is None or sources_height is None or sources_toolbar is None or sources_icon is None:
                    print(f"FAIL {label}: couldn't parse measurement from log content: {content!r}")
                    failures += 1
                    continue

                mismatches = []
                if preset_height != sources_height:
                    mismatches.append(f"preset rows {preset_height} != sources rows {sources_height}")
                if ptz_toolbar != sources_toolbar:
                    mismatches.append(f"ptzToolbar {ptz_toolbar} != sourcesToolbar {sources_toolbar}")
                if preset_toolbar != sources_toolbar:
                    mismatches.append(f"presetToolbar {preset_toolbar} != sourcesToolbar {sources_toolbar}")
                if preset_icon != sources_icon:
                    mismatches.append(f"recallIcon {preset_icon} != checkboxIcon {sources_icon}")

                summary = (
                    f"rows: preset={preset_height} sources={sources_height}; "
                    f"toolbars: ptz={ptz_toolbar} preset={preset_toolbar} sources={sources_toolbar}; "
                    f"icons: preset={preset_icon} sources={sources_icon}"
                )
                if not mismatches:
                    print(f"PASS {label}: {summary}")
                else:
                    print(f"FAIL {label}: {summary} ({'; '.join(mismatches)})")
                    failures += 1
    finally:
        # Restoring via config_set_int + relaunch rather than another
        # Settings-dialog round trip: simpler, and correct even if
        # we're exiting because the test itself seems stuck.
        quit_obs()
        if orig_density is not None:
            write_ini_value(cfg, "Density", orig_density)
        if orig_fontscale is not None:
            write_ini_value(cfg, "FontScale", orig_fontscale)

    print()
    print(f"{total - failures}/{total} passed")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
