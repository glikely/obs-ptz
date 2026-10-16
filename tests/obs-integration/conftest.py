"""Fixtures that boot one ptzsim instance and one real OBS Studio process
(plugin built from this checkout) wired together, for the whole test
session.

Device wiring is entirely config-file driven: ptzsim's cameras are
started on fixed local ports, and obs-ptz's own config.json (the file
PTZControls::LoadConfig() reads -- see src/controller/ptz-controls.cpp) is
pre-written with one device per ptzsim backend, in the format of the old
self-managed backend, which the plugin turns into a filter on a source of the
device's name (see src/controller/ptz-legacy-migration.cpp). A device is then known by
that name, in DEVICE_NAMES. OBS itself is driven at
runtime over obs-websocket (bundled since OBS 28), which every test
module uses to add a `ptz_action_source` (src/controller/ptz-action-source.c) to a
scene and make that scene current -- which is what fires the source's
configured action against the real PTZDevice, exercising the exact same
code path a user's joystick/hotkey would.
"""

import json
import os
import platform
import re
import select
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

import pytest
from obsws import EVENT_VENDORS, Client, ObsWebSocketError
from uidriver import UiDriver, UiDriverError

REPO_ROOT = Path(__file__).resolve().parents[2]

IS_WINDOWS = platform.system() == "Windows"

def ptzsim_processes():
    """(pid, parent pid, how long it has run, command) of each ptzsim running
    on this computer, whoever started it. None where there is no ps to ask
    (Windows)."""
    try:
        out = subprocess.run(["ps", "-eo", "pid=,ppid=,etime=,command="], capture_output=True, text=True,
                             check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    found = []
    for line in out.splitlines():
        fields = line.split(None, 3)
        if len(fields) < 4 or int(fields[0]) == os.getpid():
            continue
        # python itself running the module, not a shell whose command line
        # mentions it
        argv = fields[3].split()
        if os.path.basename(argv[0]).lower().startswith("python") and any(
                a == "-m" and b == "ptzsim" for a, b in zip(argv, argv[1:])):
            found.append((int(fields[0]), int(fields[1]), fields[2], fields[3]))
    return found


# The ptzsims there were when the session started, which the session didn't
# start and isn't to blame for
_strays = []


def pytest_sessionstart(session):
    """Stop, before booting anything, if a ptzsim is left over from an earlier
    run. One that is still listening answers in place of the tests' own: Sony's
    discovery answers from two cameras, a UDP port is shared, a state the tests
    didn't set up is read. The tests then fail in ways that look like nothing
    at all, only some of the time. Set PTZSIM_ALLOW_STRAYS=1 to run anyway."""
    found = ptzsim_processes()
    if not found:
        return
    _strays.extend(pid for pid, *_ in found)
    lines = "\n".join(f"  {pid} (parent {ppid}, running {etime}): {command[:100]}"
                      for pid, ppid, etime, command in found)
    message = (f"{len(found)} ptzsim left running by an earlier run:\n{lines}\n"
               "Stop them (kill " + " ".join(str(pid) for pid, *_ in found) + ") and run again, "
               "or set PTZSIM_ALLOW_STRAYS=1 to run with them.")
    if os.environ.get("PTZSIM_ALLOW_STRAYS"):
        print(f"\nWARNING: {message}", file=sys.stderr)
    else:
        pytest.exit(message, returncode=3)


def pytest_sessionfinish(session, exitstatus):
    """Say if the session left a ptzsim running, which the next run is then
    stopped by"""
    found = ptzsim_processes()
    left = [f for f in found or [] if f[0] not in _strays]
    if left:
        lines = "\n".join(f"  {pid}: {command[:100]}" for pid, _, _, command in left)
        print(f"\nWARNING: this run left {len(left)} ptzsim running:\n{lines}", file=sys.stderr)


def pytest_collection_modifyitems(config, items):
    """Skip the tests that need what Windows does not have"""
    if not IS_WINDOWS:
        return
    no_serial = pytest.mark.skip(reason="no simulated serial port on Windows")
    for item in items:
        name = item.nodeid.rsplit("::", 1)[-1]
        if "serial" in name or "pelco" in name or item.fspath.basename == "test_change_interface.py":
            item.add_marker(no_serial)


# obs-websocket RequestStatus::NotReady -- returned while OBS's frontend
# is still starting up, even after the websocket handshake has completed.
OBS_NOT_READY = 207
# obs-websocket RequestStatus::ResourceNotFound -- returned for a vendor, or a
# request of a vendor's, that nothing has registered.
OBS_NOT_FOUND = 600

# The ids the old self-managed config.json format gives each device. Nothing
# else has them: a device is its filter, and the tests refer to it by the name
# of the source that is on, DEVICE_NAMES below.
DEVICE_IDS = {
    "visca-tcp": 1,
    "visca-udp": 2,
    "visca-serial": 3,
    "pelco-d": 4,
    "pelco-p": 5,
    # Points at flaky_ptzsim (see below) rather than the shared,
    # session-scoped ptzsim_process -- test_device_status.py's
    # connect/disconnect tests need to kill and restart the camera
    # process underneath this device without disturbing the other
    # devices above.
    "visca-tcp-flaky": 6,
    # Configured with no name at all, so it's not bound to any source
    "unnamed": 12,
    # Talks ONVIF to the shared ptzsim
    "onvif": 13,
    # Talks VISCA-over-IP to sony_ptzsim, which misbehaves the way a real
    # Sony camera does (see test_visca_udp_sony.py)
    "visca-udp-sony": 14,
    # Talks VISCA-over-TCP to birddog_ptzsim, a camera with only the
    # single-value inquiries (see test_visca_no_block_inquiries.py)
    "visca-tcp-birddog": 15,
    # Turn their camera on when OBS starts and off when it closes (see
    # test_visca_power_at_obs_events.py). The first talks to power_ptzsim,
    # which is there from the start; the second to a camera that is not until
    # OBS has finished starting.
    "visca-tcp-power": 16,
    "visca-tcp-power-late": 17,
    # Has neither setting, and talks to a camera that starts off
    "visca-tcp-no-power": 18,
}

# The name of the source each device is on: the one in its config, or for a
# device that has none a placeholder the migration makes, named for its id
DEVICE_NAMES = {key: f"sim-{key}" for key in DEVICE_IDS}

# What the UI driver names OBS's main window, the PTZ Controls dock, the PTZ
# settings dialog, the dialog's state view, and the camera report dialog by
OBS_MAIN = {"class": "OBSBasic"}
PTZ_DOCK = {"class": "PTZControls"}
SETTINGS_DIALOG = {"name": "PTZSettings"}
STATE_VIEW = {"class": "PTZStateView"}
REPORT_DIALOG = {"class": "PTZCameraReportDialog"}
# The dock's controls whose enabled state follows what the camera can do
DOCK_CONTROLS = ("panTiltButton_up", "panTiltButton_home", "panTiltTouch", "zoomButton_tele", "zoomButton_wide",
                 "focusButton_auto", "focusButton_near", "focusButton_far", "focusButton_onetouch",
                 "presetListView")
DEVICE_NAMES["unnamed"] = f"PTZ Device {DEVICE_IDS['unnamed']}"


def write_preset_file(path, presets, preset_max=16, device="unused"):
    """Writes a format 1 preset export file, which an older version wrote
    (ids were the camera's slot, an int), for test_preset_import_export.py to
    feed to the real "Import Presets..." action via World.run_ui_test(): the
    dock still reads them. See on_actionPresetExport_triggered() in
    src/controller/ptz-controls.cpp for what it writes now."""
    path.write_text(json.dumps({
        "obs-ptz-preset-format": 1,
        "device": device,
        "preset_max": preset_max,
        "presets": presets,
    }))


def write_preset_file_v2(path, presets, device="unused"):
    """Writes a format 2 preset export file: `presets` in display order, each
    {"id", "store", "name"} and for a local one "values"."""
    path.write_text(json.dumps({
        "obs-ptz-preset-format": 2,
        "device": device,
        "presets": presets,
    }))


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def free_udp_port():
    """free_port() for a UDP port. The two are separate: many UDP sockets are
    open on a desktop, and about one in ten of the ports the system offers
    for TCP is already a UDP socket's"""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("", 0))
        return s.getsockname()[1]


def obs_config_root(home: Path) -> Path:
    if IS_WINDOWS:
        # A portable OBS keeps its profile in the folder it is in, see portable_obs()
        return home / "config" / "obs-studio"
    if platform.system() == "Darwin":
        return home / "Library" / "Application Support" / "obs-studio"
    return home / ".config" / "obs-studio"


def portable_obs_home(obs_binary):
    """Windows: where the OBS at obs_binary keeps its profile, for it to have a
    fresh one. OBS uses the user's own %APPDATA% however the environment says
    otherwise, unless portable_mode.txt is next to its bin folder, when it keeps
    everything in a config folder there. Only an install made for testing is
    one to put that in, so the folder the last run left is kept as config.prev
    for one run and the one before is gone."""
    home = Path(obs_binary).resolve().parents[2]
    (home / "portable_mode.txt").touch()
    config = home / "config"
    previous = home / "config.prev"
    if config.exists():
        if previous.exists():
            shutil.rmtree(previous)
        config.rename(previous)
    return home


def write_websocket_config(home: Path, ws_port, ws_password):
    """Pre-seed obs-websocket's config so it's enabled on first launch.

    The OBS_WEBSOCKET_SERVER_* env vars conftest.py also sets are what
    newer obs-websocket versions expect, but older versions ignore
    them entirely and only ever reads/writes its own settings in
    global.ini's [OBSWebSocket] section.
    Since $HOME isolation actually works on Linux (unlike macOS), we can
    just write the file directly into the isolated profile.
    """
    config_dir = obs_config_root(home)
    config_dir.mkdir(parents=True, exist_ok=True)
    # The plugin's strings are en-GB's: the language OBS is in must not be
    # one with a file of its own that takes over. And OBS has run before
    # (FirstRun): one that has not opens its Auto-Configuration Wizard, a modal
    # dialog, over a main window that then can't be closed. On Windows OBS is
    # asked to quit by closing that window, and so never quit.
    (config_dir / "user.ini").write_text("[General]\nFirstRun=true\nLanguage=en-GB\n")
    (config_dir / "global.ini").write_text(
        "[OBSWebSocket]\n"
        "FirstLoad=false\n"
        "ServerEnabled=true\n"
        f"ServerPort={ws_port}\n"
        "AlertsEnabled=false\n"
        "AuthRequired=false\n"
        f"ServerPassword={ws_password}\n")


def write_ptz_plugin_config(home: Path, ports, serial_paths):
    config_dir = obs_config_root(home) / "plugin_config" / "obs-ptz"
    config_dir.mkdir(parents=True, exist_ok=True)

    devices = [
        {
            "id": DEVICE_IDS["visca-tcp"],
            "name": "sim-visca-tcp",
            "type": "visca-over-tcp",
            "host": "127.0.0.1",
            "tcp_port": ports["visca_tcp"],
        },
        {
            "id": DEVICE_IDS["visca-udp"],
            "name": "sim-visca-udp",
            "type": "visca-over-ip",
            "host": "127.0.0.1",
            "udp_port": ports["visca_udp"],
        },
        {
            "id": DEVICE_IDS["visca-serial"],
            "name": "sim-visca-serial",
            "type": "visca",
            "serial_port": str(serial_paths["visca_serial"]),
            "address": 1,
        },
        {
            "id": DEVICE_IDS["pelco-d"],
            "name": "sim-pelco-d",
            "type": "pelco",
            "serial_port": str(serial_paths["pelco"]),
            "address": 1,
            "use_pelco_d": True,
        },
        {
            "id": DEVICE_IDS["pelco-p"],
            "name": "sim-pelco-p",
            "type": "pelco",
            "serial_port": str(serial_paths["pelco"]),
            "address": 1,
            "use_pelco_d": False,
        },
        {
            "id": DEVICE_IDS["visca-tcp-flaky"],
            "name": "sim-visca-tcp-flaky",
            "type": "visca-over-tcp",
            "host": "127.0.0.1",
            "tcp_port": ports["visca_tcp_flaky"],
        },
    ]
    devices.append({
        "id": DEVICE_IDS["visca-udp-sony"],
        "name": "sim-visca-udp-sony",
        "type": "visca-over-ip",
        "host": "127.0.0.1",
        "udp_port": ports["visca_udp_sony"],
    })
    devices.append({
        "id": DEVICE_IDS["visca-tcp-birddog"],
        "name": "sim-visca-tcp-birddog",
        "type": "visca-over-tcp",
        "host": "127.0.0.1",
        "tcp_port": ports["visca_tcp_birddog"],
    })
    for key, port in (("visca-tcp-power", ports["visca_tcp_power"]),
                      ("visca-tcp-power-late", ports["visca_tcp_power_late"])):
        devices.append({
            "id": DEVICE_IDS[key],
            "name": f"sim-{key}",
            "type": "visca-over-tcp",
            "host": "127.0.0.1",
            "tcp_port": port,
            "power_on_at_startup": True,
            "power_off_at_shutdown": True,
        })
    devices.append({
        "id": DEVICE_IDS["visca-tcp-no-power"],
        "name": "sim-visca-tcp-no-power",
        "type": "visca-over-tcp",
        "host": "127.0.0.1",
        "tcp_port": ports["visca_tcp_no_power"],
    })
    devices.append({
        "id": DEVICE_IDS["onvif"],
        "name": "sim-onvif",
        "type": "onvif",
        "host": "127.0.0.1",
        "port": ports["onvif_http"],
        "username": "admin",
        "password": "",
    })
    devices.append({
        "id": DEVICE_IDS["unnamed"],
        "name": "",
        "type": "visca-over-ip",
        "host": "127.0.0.1",
        "udp_port": ports["unused_udp"],
    })
    (config_dir / "config.json").write_text(json.dumps({"devices": devices}))
    write_visca_profiles(config_dir / "visca-profiles")


# A user's VISCA command sets (see test_visca_user_profiles.py), by file name.
# The plugin reads them once, so they are there before OBS starts.
VISCA_PROFILES = {
    # A new state value, set and read with the white balance mode's command
    # and inquiry, one read from further into a reply than its inquiry is
    # long, a new trigger, and a built-in state value and the zoom drive
    # taken away
    "test-user.json": {
        "id": "test-user",
        "name": "Test camera",
        "models": ["0123:0001"],
        "remove": ["low_latency", "zoom_drive"],
        "controls": [{
            "key": "user_wb",
            "set": {"cmd": "8101043500ff", "args": [{"type": "u4", "offset": 4}]},
            "reads": [{"cmd": "81090435ff", "results": [{"type": "u4", "offset": 2}]}],
        }, {
            "key": "user_zoom",
            "reads": [{"cmd": "81097e7e00ff", "results": [{"type": "u16", "offset": 2}]}],
        }],
        "triggers": {"user_home": "81010604ff"},
    },
    # One that extends another, read before it
    "a-extended.json": {
        "id": "test-user-extended",
        "extends": "test-user",
        "models": ["0123:0002"],
        "remove": ["user_wb"],
    },
    # The BirdDog P100's, which is shipped with the plugin, as it is but
    # also for a test model, and without one of the state values, which
    # replaces the shipped one
    "birddog-p100.json": {
        **json.loads((REPO_ROOT / "src" / "drivers" / "visca" / "visca-profiles" / "birddog-p100.json").read_text()),
        "models": ["0109:2020", "0123:0004"],
        "remove": ["tally_preview", "low_latency"],
    },
    # One that only changes a control: that the AE mode can't be set
    "control-only.json": {
        "id": "test-control-only",
        "models": ["0123:0005"],
        "controls": [{"key": "ae_mode", "set": None}],
    },
    # One that takes the camera's presets away, and leaves the absolute moves
    # that the local ones are made of
    "local-only.json": {
        "id": "test-local-only",
        "models": ["0123:0008"],
        "remove": ["memory_recall"],
    },
    # One with a misspelt key, which would otherwise do nothing: left out
    "misspelt.json": {
        "id": "test-misspelt",
        "models": ["0123:0007"],
        "remove_inquiry": ["81097e7e00ff"],
    },
    # One with a command that isn't hex, which is left out
    "broken.json": {
        "id": "broken",
        "models": ["0123:0003"],
        "remove": ["low_latency"],
        "triggers": {"user_x": "8101zzff"},
    },
}


def write_visca_profiles(profile_dir: Path):
    profile_dir.mkdir(parents=True, exist_ok=True)
    for name, profile in VISCA_PROFILES.items():
        (profile_dir / name).write_text(json.dumps(profile))


def output_log(name):
    """Where a child process's output goes: a file, kept for looking at after
    a failure. Not a pipe: nothing reads one, so once it fills up the child
    blocks on its next write -- a whole ptzsim, or OBS's main thread, logging
    -- and the tests time out waiting for it. Close it after starting the
    child, which has its own copy."""
    fd, path = tempfile.mkstemp(prefix=f"{name}-", suffix=".log")
    out = os.fdopen(fd, "w")
    out.path = Path(path)  # the file's own name is its descriptor
    return out


def wait_for_port(host, port, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=1):
                return
        except OSError:
            time.sleep(0.2)
    raise TimeoutError(f"nothing listening on {host}:{port} after {timeout}s")


def start_xvfb(timeout=10):
    """Starts an Xvfb on a free display, in its own process group; returns
    it and its DISPLAY. -displayfd has Xvfb pick the display and write its
    number to the pipe once it is ready for clients."""
    read_fd, write_fd = os.pipe()
    try:
        with output_log("xvfb") as out:
            proc = subprocess.Popen(
                ["Xvfb", "-displayfd", str(write_fd), "-screen", "0", "1920x1080x24", "-nolisten", "tcp"],
                stdout=out, stderr=subprocess.STDOUT, pass_fds=(write_fd,), start_new_session=True)
        os.close(write_fd)
        write_fd = None
        number = b""
        deadline = time.time() + timeout
        while not number.endswith(b"\n"):
            ready, _, _ = select.select([read_fd], [], [], max(0, deadline - time.time()))
            chunk = os.read(read_fd, 16) if ready else b""
            if not chunk:
                stop_process_group(proc, timeout=5)
                raise RuntimeError("Xvfb never said which display it is on")
            number += chunk
        return proc, f":{number.decode().strip()}"
    finally:
        os.close(read_fd)
        if write_fd is not None:
            os.close(write_fd)


def stop_process_tree(proc, timeout):
    """stop_process_group() where there are no process groups: asks proc's
    windows to close, as taskkill does without /F, and when it has not exited,
    ends it and what it started. Returns whether proc exited on its own.

    It is asked more than once. With another window of its own open, the PTZ
    settings dialog that a test left, say, the first request closes that window
    and OBS stays: it is the next that closes the main window, and OBS with it."""
    deadline = time.time() + timeout
    exited = False
    while not exited and time.time() < deadline:
        subprocess.run(["taskkill", "/PID", str(proc.pid), "/T"], capture_output=True)
        try:
            proc.wait(timeout=1)
            exited = True
        except subprocess.TimeoutExpired:
            pass
    if not exited:
        subprocess.run(["taskkill", "/PID", str(proc.pid), "/T", "/F"], capture_output=True)
    proc.wait()
    return exited


def stop_process_group(proc, timeout, sig=signal.SIGTERM):
    """Sends sig to proc (started with start_new_session=True) and waits for
    it to exit, then ends whatever else is left in its process group.
    Returns whether proc exited on its own; SIGKILLs it if not."""
    if IS_WINDOWS:
        return stop_process_tree(proc, timeout)
    proc.send_signal(sig)
    try:
        proc.wait(timeout=timeout)
        exited = True
    except subprocess.TimeoutExpired:
        exited = False
    pgid = proc.pid
    for leftover_sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.killpg(pgid, leftover_sig)
        except ProcessLookupError:
            break
        # Gone once nothing in the group answers signal 0 any more (proc is
        # still a group member until reaped, so reap it first).
        deadline = time.time() + 5
        try:
            proc.wait(timeout=5)
            while time.time() < deadline:
                os.killpg(pgid, 0)
                time.sleep(0.1)
        except (ProcessLookupError, subprocess.TimeoutExpired):
            pass
    proc.wait()
    return exited


class World:
    def __init__(self, ws, debug_url, device_names):
        self.ws = ws
        # A second connection, subscribed to obs-websocket's Vendors events:
        # set by obs_world. What a test waits for is
        # read from here, so that the requests on ws don't have to keep it.
        self.events = None
        # And a third, for record_preset_signals() and preset_signals(), which
        # take every event that comes on it
        self.signals = None
        self._recorded = []
        self._recording = set()
        self._record_start = time.monotonic()
        # What open_settings_dialog() started counting from: settings_dialog()
        # counts the settings view's redraws and the state view's changes since
        self._settings_watch = None
        self._state_updates_at_open = 0
        # Where and how to open another connection: set by obs_world
        self.ws_url = None
        self.ws_password = None
        self.debug_url = debug_url
        self.device_names = device_names
        # The plugin's config directory, for tests that look at what it keeps there
        self.config_dir = None
        # Set by obs_world: quits OBS and starts it again on the same profile
        self.restart_obs = None
        # What the OBS that restart_obs() quit exited with
        self.exit_code = None
        # Set by obs_world: the file the running OBS's output goes to
        self.obs_log = None
        # Somewhere for the small files a test asks the harness to write
        self.scratch = Path(tempfile.mkdtemp(prefix="ptz-world-"))
        self._scene_counter = 0

    def state(self):
        with urllib.request.urlopen(self.debug_url, timeout=5) as resp:
            return json.loads(resp.read())

    def set_move_time(self, seconds):
        """How long the shared ptzsim's moves across the whole range take from
        now on (0: at once, which is how it starts), as its --move-time says"""
        url = self.debug_url.rsplit("/", 1)[0] + "/move_time"
        body = json.dumps({"seconds": seconds}).encode("utf-8")
        request = urllib.request.Request(url, data=body, method="POST",
                                         headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(request, timeout=5):
            pass

    def wait_for_state(self, predicate, timeout=5, interval=0.1):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            last = self.state()
            if predicate(last):
                return last
            time.sleep(interval)
        raise AssertionError(f"state never matched predicate; last seen: {last}")

    def trigger_action(self, device_name, action, preset_id=0, pan_speed=0.0, tilt_speed=0.0):
        """Create a ptz_action_source in a fresh scene and make that scene
        current, which fires the action via PTZ_ACTION_TRIGGER_PROGRAM_ACTIVE
        (the source's default trigger -- see ptz_action_source_activate())."""
        # The action names the device's filter, by UUID
        found = self.wait_for_device_by_name(device_name, self.scratch / "action-device.json", lambda r: r["found"])
        self._scene_counter += 1
        scene = f"ptzsim-test-{self._scene_counter}"
        source = f"{scene}-action"
        self.ws.call("CreateScene", {"sceneName": scene})
        self.ws.call("CreateInput", {
            "sceneName": scene,
            "inputName": source,
            "inputKind": "ptz_action_source",
            "inputSettings": {
                "trigger": 0,  # PTZ_ACTION_TRIGGER_PROGRAM_ACTIVE
                "device_uuid": found["uuid"],
                "action": action,
                "preset_id": preset_id,
                "pan_speed": pan_speed,
                "tilt_speed": tilt_speed,
            },
        })
        self.ws.call("SetCurrentProgramScene", {"sceneName": scene})

    def create_scene(self):
        """Creates an empty scene that cleanup_scenes() removes again, and
        returns its name."""
        self._scene_counter += 1
        scene = f"ptzsim-test-{self._scene_counter}"
        self.ws.call("CreateScene", {"sceneName": scene})
        return scene

    def _query_device_source(self, out_file=None, device=None):
        """What is known of a device, by its UUID, its name or the name of its source:
        {"found"} alone if there is none, otherwise also "uuid", "name" (as the dock's camera list shows it), the "source"
        it is on and its "source_uuid" ("bound" says there is one), as the PTZ API
        says them: the websocket binding asks the device's ptz_get_parent_source, so
        "proc_bound", "proc_source" and "proc_source_uuid" are the same; and "live",
        "preview" and "locked", as the dock's camera list has them. out_file is
        not used: it was where the harness wrote all this."""
        self.need_ui_driver()
        try:
            listed = self.ws.vendor("obs-ptz", "ptz_list_devices")["devices"]
        except ObsWebSocketError as e:
            # With the binding's access off (a test does that) it answers no request: the
            # dock's camera list says the devices, by the names it shows
            if e.status.get("code") != 600:
                raise
            rows, _, _ = self.ui.rows(OBS_MAIN, PTZ_DOCK, "deviceList", roles=["display", "uuid"])
            listed = [{"device": r["uuid"], "name": r["display"], "source": r["display"]} for r in rows]
        entry = None
        for key in ("device", "name", "source"):
            entry = next((e for e in listed if e[key] == device), None)
            if entry:
                break
        result = {"found": entry is not None}
        if not entry:
            return result
        source_uuid = ""
        if entry["source"]:
            inputs = self.ws.call("GetInputList")["inputs"]
            source_uuid = next((i["inputUuid"] for i in inputs if i["inputName"] == entry["source"]), "")
        rows, _, _ = self.ui.rows(OBS_MAIN, PTZ_DOCK, "deviceList", roles=["display", "uuid", "live", "preview", "locked"])
        row = next((r for r in rows if r["uuid"] == entry["device"]), {})
        bound = bool(entry["source"])
        result.update({
            "uuid": entry["device"], "name": row.get("display", entry["name"]),
            "bound": bound, "source": entry["source"], "source_uuid": source_uuid,
            "proc_bound": bound, "proc_source": entry["source"], "proc_source_uuid": source_uuid,
            "live": bool(row.get("live")), "preview": bool(row.get("preview")), "locked": bool(row.get("locked")),
        })
        return result

    def device_source(self, device_name, out_file):
        """Fetches which OBS source device_name is bound to, and the
        device's name, from the websocket binding's device list and the dock's
        camera list (see _query_device_source())."""
        return self._query_device_source(out_file, device=device_name)

    def device_by_name(self, name, out_file):
        """device_source() under the name of its source, for a device a test
        made, which is the name it asks the device by anyway. Nothing but
        {"found": False} is there if there's no such device."""
        return self._query_device_source(out_file, device=name)

    def wait_for_device_source(self, device_name, out_file, predicate, timeout=5, interval=0.2):
        """Polls device_source() until predicate(result) is true -- name
        changes and the device list's cached live/locked state reach it
        asynchronously, via signals queued onto the GUI thread."""
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            last = self.device_source(device_name, out_file)
            if predicate(last):
                return last
            time.sleep(interval)
        raise AssertionError(f"device {device_name} source never matched predicate; last seen: {last}")

    def set_device_locked(self, device_name, locked):
        """Locks or unlocks a device as a click on its padlock in the dock's camera
        list does: the list's model is given the new value for the device's row"""
        self.need_ui_driver()
        uuid = self.device_uuid(device_name)
        rows, _, _ = self.ui.rows(OBS_MAIN, PTZ_DOCK, "deviceList", roles=["uuid", "locked"])
        row = next(i for i, r in enumerate(rows) if r["uuid"] == uuid)
        # The model takes only a change: a click on a padlock already so does nothing
        if bool(rows[row]["locked"]) != bool(locked):
            self.ui.set_data(OBS_MAIN, PTZ_DOCK, "deviceList", row=row, value=bool(locked), role="locked")

    def wait_for_device_by_name(self, name, out_file, predicate, timeout=5, interval=0.2):
        """wait_for_device_source() for a device found by name."""
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            last = self.device_by_name(name, out_file)
            if predicate(last):
                return last
            time.sleep(interval)
        raise AssertionError(f"device {name!r} never matched predicate; last seen: {last}")

    def cleanup_scenes(self):
        for i in range(1, self._scene_counter + 1):
            try:
                self.ws.call("RemoveScene", {"sceneName": f"ptzsim-test-{i}"})
            except ObsWebSocketError:
                pass

    def run_ui_test(self, cmd, connection=None, **params):
        """Dispatches a tests/ui-harness/ test by name (matching one of
        its registerTest() calls) via the "obs-ptz" vendor's
        "ui_test_run" request -- the same request scripts/
        test_preset_row_sizing.py uses for the appearance_row_sizing/
        measure_now tests. obs-websocket's vendorRequestCallback()
        (tests/ui-harness/ui-test-harness.cpp) only ever reads params as
        strings (obs_data_item_get_string on every field), so stringify
        them here rather than relying on JSON's own int/float types.

        This is fire-and-forget: the harness only acknowledges receipt
        ("accepted": true) and runs the actual test later, queued onto
        the GUI thread -- see vendorRequestCallback()'s own comment for
        why. Callers that need to observe the test's effect (e.g. a file
        it wrote) have to poll for it, e.g. with wait_for() below.

        It is asked on the connection for requests, or on `connection`, for a
        test of what a connection does with what the request causes."""
        try:
            return (connection or self.ws).vendor("obs-ptz", "ui_test_run",
                                                  {"cmd": cmd, **{k: str(v) for k, v in params.items()}})
        except ObsWebSocketError as e:
            # obs-websocket's ResourceNotFound: the plugin was built without
            # the harness (-DENABLE_UI_TESTS=ON), as a release is. What needs
            # no harness, the PTZ API over obs-websocket, is still tested.
            if e.status.get("code") == OBS_NOT_FOUND:
                pytest.skip("needs the UI test harness, which this build of the plugin has not")
            raise

    def device_uuid(self, device):
        """The UUID of a device a test names by it, or by the name of the source
        the device is on; None if there is no such device"""
        listed = self.ws.vendor("obs-ptz", "ptz_list_devices")["devices"]
        for key in ("device", "source"):
            for entry in listed:
                if entry[key] == device:
                    return entry["device"]
        return None

    def _signal_ms(self, at=None):
        """A time, now unless given, in ms on the clock of the recorded signals' "ms"""
        return ((time.monotonic() if at is None else at) - self._record_start) * 1000

    def need_harness(self):
        """Skips the test if the plugin was built without the UI test harness,
        for a test that needs something of the harness's before it asks it for
        anything: the source kinds it registers, say"""
        self.run_ui_test("nothing")

    @property
    def ui(self):
        """The UI driver (tests/ui-driver/), over the connection for requests,
        whichever that is after a restart"""
        return UiDriver(self.ws)

    def need_ui_driver(self):
        """Skips the test if OBS hasn't the UI driver, a plugin of its own that
        is only built with -DENABLE_UI_TESTS=ON"""
        if not self.ui.available():
            pytest.skip("needs the UI driver plugin, which this build has not")

    def call_proc(self, device, proc, args=None, returns=None):
        """Calls proc on device, as a client of obs-websocket does: the "obs-ptz"
        vendor's request of that name (see "WebSocket binding" in
        docs/ptz-device-api.md), with the calldata fields in args, a dict or list
        being the obs_data_t a ptr field takes. returns is what it gives back as
        "return": "string", "int", "bool" or "data" (a dict). Returns
        {"called", "return", "args", "ms"}: "called" is False if there is no such
        device, or it has no such proc; "args" has the dicts passed in, as the
        proc left them; "ms" is when it was called, on the clock of the recorded
        signals' "ms"""
        args = args or {}
        result = {"called": False, "args": {}, "ms": self._signal_ms()}
        uuid = self.device_uuid(device)
        if uuid is None:
            return result
        try:
            answer = self.ws.vendor("obs-ptz", proc, {"device": uuid, **args})
        except ObsWebSocketError as e:
            # the device hasn't the proc, or nothing has: obs-websocket's ResourceNotFound
            if e.status.get("error") == "no_such_proc" or e.status.get("code") == 600:
                return result
            raise
        result["called"] = True
        result["args"] = {key: answer[key] for key, value in args.items()
                          if isinstance(value, (dict, list)) and key in answer}
        if returns:
            result["return"] = answer.get("return", {"string": "", "int": 0, "bool": False, "data": {}}[returns])
        return result

    def _call_void(self, device, proc, args):
        """call_proc() for a proc that gives nothing back, which device has to have"""
        assert self.call_proc(device, proc, args)["called"], f"{device} has no {proc}"

    def set_device_state(self, device, **state):
        """Asks device to change the state values given, with ptz_request_state,
        as the settings dialog's state view does"""
        self._call_void(device, "ptz_request_state", {"state": state})

    def move_device(self, device, mode="abs", **axes):
        """Moves device to where, or by how much, the axes given say, with
        ptz_move_abs, or ptz_move_rel for mode="rel". An axis left out is left
        alone."""
        self._call_void(device, "ptz_move_abs" if mode == "abs" else "ptz_move_rel",
                        {axis: float(value) for axis, value in axes.items()})

    def trigger_device(self, device, name):
        """Has device do the one-shot action `name`, with ptz_trigger"""
        self._call_void(device, "ptz_trigger", {"name": name})

    def update_device(self, device, **changes):
        """Changes the settings of device's filter as the settings dialog's Apply
        does: the whole of them, the kind's defaults and what the filter has, with
        `changes` over them, so that what is not changed keeps its value. Over
        obs-websocket, as SetSourceFilterSettings, which updates the filter as
        PTZListModel::update() does."""
        uuid = self.device_uuid(device)
        assert uuid is not None, f"no device {device}"
        listed = next(e for e in self.ws.vendor("obs-ptz", "ptz_list_devices")["devices"] if e["device"] == uuid)
        where = {"sourceName": listed["source"], "filterName": listed["name"]}
        current = self.ws.call("GetSourceFilter", where)
        defaults = self.ws.call("GetSourceFilterDefaultSettings",
                                {"filterKind": current["filterKind"]})["defaultFilterSettings"]
        settings = {**defaults, **current["filterSettings"], **changes}
        # The marks of a driver's kind are no setting of the device (see
        # PTZListModel::save())
        for mark in ("ptz_api_version_major", "ptz_api_version_minor"):
            settings.pop(mark, None)
        self.ws.call("SetSourceFilterSettings", {**where, "filterSettings": settings, "overlay": True})

    def api_version(self, device):
        """What device's ptz_get_api_version says, as {"called", "major", "minor"}"""
        uuid = self.device_uuid(device)
        if uuid is None:
            return {"called": False}
        return {"called": True, **self.ws.vendor("obs-ptz", "ptz_get_api_version", {"device": uuid})}

    def _collect_preset_signals(self, settle=0.05):
        """Takes what has come on the signals' connection, and keeps the
        ptz_preset_* events of the devices being recorded. An event is sent apart
        from the answer to the request that caused it, and can come a moment
        after it: settle is how long that moment is waited for."""
        time.sleep(settle)
        for event in self.signals.take("VendorEvent"):
            data = event["eventData"]
            fields = data.get("eventData") or {}
            if (data.get("vendorName") != "obs-ptz" or not data.get("eventType", "").startswith("ptz_preset_")
                    or fields.get("device") not in self._recording):
                continue
            self._recorded.append({"signal": data["eventType"],
                                   **{key: value for key, value in fields.items() if key != "device"},
                                   "ms": self._signal_ms(event["at"])})

    def record_preset_signals(self, device, keep=False):
        """Starts recording device's ptz_preset_* signals, as the events a client
        of obs-websocket is sent, from nothing, or with keep only makes sure it is
        being recorded, and keeps what has been"""
        # What was said before recording began is not to be taken for what
        # came after: a little longer for it, on a slow machine
        self._collect_preset_signals(settle=0.05 if keep else 0.3)
        if not keep:
            self._recorded = []
            self._recording = set()
            self._record_start = time.monotonic()
        uuid = self.device_uuid(device)
        if uuid is not None:
            self._recording.add(uuid)

    def preset_signals(self, with_create_done=False):
        """What the signals recorded since record_preset_signals() have said, as
        [{"signal", "id", "request", "changed", "ms"}...], the fields each has, in
        the order they came, "ms" being when, since recording began.
        ptz_preset_create_done is only in it with_create_done: create_preset() waits for it,
        and what a test asks of the others is not its to see"""
        self._collect_preset_signals()
        events = list(self._recorded)
        return events if with_create_done else [e for e in events if e["signal"] != "ptz_preset_create_done"]

    def create_preset(self, device, name="", store="camera", timeout=10):
        """Asks device for a preset with ptz_preset_create, which does not wait for the
        camera, and waits for ptz_preset_create_done to say it has: returns the id of the preset
        made, or "" if the device would not take the request, or made none"""
        self.record_preset_signals(device, keep=True)
        result = self.call_proc(device, "ptz_preset_create", {"name": name, "store": store}, returns="string")
        assert result["called"], "the device has no ptz_preset_create"
        request = result["return"]
        if not request:
            return ""
        found = []

        def done():
            found[:] = [e for e in self.preset_signals(with_create_done=True)
                        if e["signal"] == "ptz_preset_create_done" and e["request"] == request]
            return bool(found)

        self.wait_for(done, timeout=timeout)
        return found[0]["id"]

    def select_in_dock(self, device):
        """Selects a device in the PTZ Controls dock's camera list, as a click does"""
        self.need_ui_driver()
        uuid = self.device_uuid(device)
        assert uuid is not None, f"no device {device}"
        self.ui.select(OBS_MAIN, PTZ_DOCK, "deviceList", value=uuid, role="uuid")

    def scenes_settled(self, program=None, preview=None):
        """Waits until OBS has made `program` its program scene, and `preview` its preview
        scene, which it does on its main thread after obs-websocket has answered the
        request to; and then for the main thread to have done what it does with the news,
        such as the dock putting its cameras' locks back to whether they are live, by
        having the UI driver ask it for something"""
        def settled():
            ok = program is None or self.ws.call("GetCurrentProgramScene")["currentProgramSceneName"] == program
            ok = ok and (preview is None or
                         self.ws.call("GetCurrentPreviewScene")["currentPreviewSceneName"] == preview)
            # OBS has the new program scene as its transition to it starts, a fade on a
            # new profile, and says so only once it has finished: obs-websocket's cursor
            # is 1 when no transition is running
            return ok and self.ws.call("GetCurrentSceneTransitionCursor")["transitionCursor"] >= 1.0

        self.wait_for(settled)
        # The driver's ping is answered on the main thread; without the driver, that
        # part of the wait isn't had
        self.ui.available()

    def save_dock(self):
        """Has OBS save the scene collection, and so the dock's settings, as it does
        when a scene changes: its SaveProject()"""
        self.need_ui_driver()
        self.ui.invoke(OBS_MAIN, method="SaveProject")

    def dock_selected(self, device):
        """Whether `device` is the one selected in the PTZ Controls dock's camera list"""
        self.need_ui_driver()
        uuid = self.device_uuid(device)
        rows, current, _ = self.ui.rows(OBS_MAIN, PTZ_DOCK, "deviceList", roles=["uuid"])
        return uuid is not None and 0 <= current < len(rows) and rows[current]["uuid"] == uuid

    def set_autoselect(self, enabled):
        """Turns the dock's autoselecting of a camera on or off, as the checkbox in its menu does"""
        self.need_ui_driver()
        self.ui.invoke(OBS_MAIN, PTZ_DOCK, method="setAutoselectEnabled", args=[enabled])

    def select_scene_item(self, name):
        """Selects the item `name` in OBS's Sources list, as a click does. A new item
        starts out selected, and selecting a selected row is no event, so the
        selection is cleared first."""
        self.need_ui_driver()
        self.ui.invoke(OBS_MAIN, "sources", method="clearSelection")
        self.ui.select(OBS_MAIN, "sources", value=name, role="accessibleText")

    def _preset_file_action(self, device, action, path):
        """Selects device in the dock, then does what its "Export Presets..." or
        "Import Presets..." does once the file is chosen: the slot the action
        triggers, with the file, as the native file dialog can't be driven.
        Queued, so that a message box it shows doesn't hold up the answer."""
        self.select_in_dock(device)
        self.ui.invoke(OBS_MAIN, PTZ_DOCK, method=f"on_actionPreset{action}_triggered", args=[str(path)],
                       queued=True)

    def export_presets(self, device, path):
        """Exports device's presets to path, as the dock's "Export Presets..."
        does, and gives back what it wrote, once it has"""
        path.unlink(missing_ok=True)
        self._preset_file_action(device, "Export", path)
        self.wait_for(lambda: path.exists() and path.read_text())
        return json.loads(path.read_text())

    def import_presets(self, device, path):
        """Imports the presets in path to device, as the dock's "Import
        Presets..." does. It is done once OBS's main thread gets to it: wait for
        what it changes."""
        self._preset_file_action(device, "Import", path)

    def device_settings(self, device_name, out_file):
        """Fetches, as three sets of key names, what device_name's settings
        properties edit ("property_keys"), what saving it writes
        ("save_keys"), and what its PTZ filter (if it has one) would
        persist ("filter_keys"), via tests/ui-harness/device-settings-test.cpp's
        "get_device_settings" test. Also returns what saving it wrote, with
        its values, as "saved", and what each string list property offers,
        as "lists": {key: [value, ...]}, and the string defaults of its filter's live
        settings, which are not saved, as "filter_defaults"."""
        if out_file.exists():
            out_file.unlink()
        self.run_ui_test("get_device_settings", device=device_name, filename=str(out_file))
        self.wait_for(out_file.exists)
        raw = json.loads(out_file.read_text())
        keys = {name: {e["key"] for e in raw.get(name, [])} for name in ("property_keys", "save_keys", "filter_keys")}
        keys["saved"] = raw.get("saved", {})
        keys["filter_defaults"] = raw.get("filter_defaults", {})
        keys["lists"] = {e["key"]: [v["value"] for v in e["values"]] for e in raw.get("lists", [])}
        return keys

    def camera_report(self, device_name, out_file=None):
        """Fetches device_name's camera report, or None if it has none, with
        ptz_get_camera_report. out_file is not used: it was where the report was
        written when it was fetched through the UI test harness."""
        uuid = self.device_uuid(device_name)
        assert uuid is not None, f"no device {device_name}"
        report = self.ws.vendor("obs-ptz", "ptz_get_camera_report", {"device": uuid}).get("report")
        return json.loads(report) if report else None

    def device_state(self, device_name, out_file=None):
        """Fetches device_name's whole transient state ("state"), as a client of
        obs-websocket does, with ptz_get_state, and its statistics, which are
        read apart from the state but the tests that look at them ask for with
        it, as "statistics" in it. out_file is not used: it was where the state
        was written when it was fetched through the UI test harness."""
        uuid = self.device_uuid(device_name)
        assert uuid is not None, f"no device {device_name}"
        state = self.ws.vendor("obs-ptz", "ptz_get_state", {"device": uuid})["state"]
        state["statistics"] = self.ws.vendor("obs-ptz", "ptz_get_statistics", {"device": uuid})["statistics"]
        return {"state": state}

    def wait_for_device_state(self, device_name, out_file, predicate, timeout=5, interval=0.2):
        """Polls device_state() until predicate(result) is true."""
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            last = self.device_state(device_name, out_file)
            if predicate(last):
                return last
            time.sleep(interval)
        raise AssertionError(f"device {device_name} state never matched predicate; last seen: {last}")

    def _open_settings_window(self):
        """Opens the PTZ settings dialog as a user does, from OBS's Tools menu"""
        self.ui.invoke(OBS_MAIN, "menuTools", {"class": "QAction", "text": "PTZ Devices"}, method="trigger")

    def open_settings_dialog(self, device):
        """Opens the PTZ settings dialog on a device, as a user does: from OBS's
        Tools menu, then choosing the device in the dialog's list, then its
        Devices tab. Counts the settings view's redraws and the state view's
        changes from now (see settings_dialog()). Skips the test if OBS hasn't
        the UI driver, as the dialog's other helpers do."""
        self.need_ui_driver()
        uuid = self.device_uuid(device)
        assert uuid is not None, f"no device {device}"
        self._open_settings_window()
        self.ui.select(SETTINGS_DIALOG, "deviceList", value=uuid, role="uuid")
        pages = [tab["page"] for tab in self.ui.get1(SETTINGS_DIALOG, "tabWidget", name="tabs")]
        self.ui.set(SETTINGS_DIALOG, "tabWidget", currentIndex=pages.index("devicesTab"))
        self._settings_watch = self.ui.watch(SETTINGS_DIALOG, {"class": "OBSPropertiesView"},
                                             signal="PropertiesRefreshed")
        self._state_updates_at_open = self.ui.get1(SETTINGS_DIALOG, STATE_VIEW, name="updateCount")

    def settings_dialog(self, out_file=None):
        """What the open PTZ settings dialog shows, read from its widgets
        through the UI driver (open it first, with open_settings_dialog()):

          settings_keys - the keys of the settings the settings view edits
          shown, state_keys - every value the state view shows, by key, and its keys
          wb_mode, connected, pan, tilt, zoom, focus - some of those
          header - the status line above the views, with its markup
          settings_refreshes, state_updates - how often the settings view
              redrew, and the state view changed, since it was opened
          apply_enabled - whether Apply is enabled: there are edits to apply
          tabs - the visible tabs, [{"name"}...]
          state_widgets - how many widgets the state view is made of
          wb_widget - the id of its white balance list, None if it has none:
              the same id is the same widget, kept rather than replaced
          diagnostics_visible - whether the diagnostics are shown at all
              (they are on a tab of their own, which may not be the one showing)

        out_file is not used: it was where the harness wrote all this."""
        self.need_ui_driver()
        ui = self.ui
        shown = ui.get1(SETTINGS_DIALOG, STATE_VIEW, name="shownValues") or {}
        try:
            wb_widget = ui.find(SETTINGS_DIALOG, STATE_VIEW, "wb_mode")[0]["id"]
        except UiDriverError as e:
            if e.code != "not_found":
                raise
            wb_widget = None
        try:
            state_widgets = len(ui.find(SETTINGS_DIALOG, STATE_VIEW, {"class": "QWidget"}, all=True))
        except UiDriverError as e:
            if e.code != "not_found":
                raise
            state_widgets = 0
        dialog = {
            "settings_keys": set(ui.get1(SETTINGS_DIALOG, name="settingsKeys") or []),
            "shown": shown,
            "state_keys": set(shown),
            "wb_mode": shown.get("wb_mode", 0),
            "connected": shown.get("connected", False),
            "header": ui.get1(SETTINGS_DIALOG, "statusHeader", name="text"),
            "settings_refreshes": ui.count(self._settings_watch) if self._settings_watch else 0,
            "state_updates": ui.get1(SETTINGS_DIALOG, STATE_VIEW, name="updateCount") - self._state_updates_at_open,
            "apply_enabled": ui.get1(SETTINGS_DIALOG, "applyButton", name="enabled"),
            "tabs": [{"name": tab["text"]} for tab in ui.get1(SETTINGS_DIALOG, "deviceTabs", name="tabs")
                     if tab["visible"]],
            "state_widgets": state_widgets,
            "wb_widget": wb_widget,
            "diagnostics_visible": not ui.get1(SETTINGS_DIALOG, "diagnostics", name="hidden"),
        }
        for axis in ("pan", "tilt", "zoom", "focus"):
            dialog[axis] = shown.get(axis, 0.0)
        return dialog

    def edit_dialog_state(self, **fields):
        """Edits the state view's fields as a user does, each a state key and the
        value to give it: picks it in a list, clicks a checkbox that isn't
        already that way, or enters a number. A field that isn't there, or a
        list without that value, is left: a user couldn't edit it either."""
        self.need_ui_driver()
        ui = self.ui
        for key, value in fields.items():
            field = (SETTINGS_DIALOG, STATE_VIEW, key)
            try:
                kind = ui.find(*field)[0]["class"]
                if kind == "QComboBox":
                    ui.activate(*field, data=value)
                elif kind == "QCheckBox":
                    if ui.get1(*field, name="checked") != bool(value):
                        ui.invoke(*field, method="click")
                elif kind == "QSpinBox":
                    ui.set(*field, value=value)
                else:
                    raise AssertionError(f"the state view's {key} is a {kind}, which isn't edited")
            except UiDriverError as e:
                if e.code != "not_found":
                    raise

    def press_dialog_button(self, name):
        """Presses one of the PTZ settings dialog's buttons, by its object
        name ("cameraReport", say), as a user does"""
        self.need_ui_driver()
        self.ui.invoke(SETTINGS_DIALOG, name, method="click")

    def _settings_window(self):
        """Opens the PTZ settings dialog, if it isn't open"""
        self.need_ui_driver()
        try:
            self.ui.find(SETTINGS_DIALOG)
        except UiDriverError as e:
            if e.code != "not_found":
                raise
            self._open_settings_window()

    def set_live_move_lock(self, enabled):
        """Turns "Block Live Camera Moves" on or off, with its checkbox in the
        PTZ settings dialog, which is opened if it isn't"""
        self._settings_window()
        if self.ui.get1(SETTINGS_DIALOG, "livemoveCheckBox", name="checked") != enabled:
            self.ui.invoke(SETTINGS_DIALOG, "livemoveCheckBox", method="click")

    def wait_for_settings_dialog(self, out_file, predicate, timeout=5, interval=0.2):
        """Polls settings_dialog() until predicate(result) is true."""
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            last = self.settings_dialog(out_file)
            if predicate(last):
                return last
            time.sleep(interval)
        raise AssertionError(f"settings dialog never matched predicate; last seen: {last}")

    def device_status(self, device_name, out_file=None):
        """Fetches device_name's {"connected", "power_on", "focus_af_enabled",
        "wb_mode"} from its state (ptz_get_state), a value it doesn't report
        being false or 0: "wb_mode" is VISCA's alone. "connected" is the link's
        own state, apart from "power_on", what the camera says of its power.
        out_file is not used: it was where the status was written when it was
        fetched through the UI test harness."""
        state = self.device_state(device_name)["state"]
        return {"connected": bool(state.get("connected", False)),
                "power_on": bool(state.get("power_on", False)),
                "focus_af_enabled": bool(state.get("focus_af_enabled", False)),
                "wb_mode": state.get("wb_mode", 0)}

    def wait_for_device_status(self, device_name, out_file, predicate, timeout=5, interval=0.2):
        """Polls device_status() until predicate(status) is true."""
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            last = self.device_status(device_name, out_file)
            if predicate(last):
                return last
            time.sleep(interval)
        raise AssertionError(f"device {device_name} status never matched predicate; last seen: {last}")

    def dock_controls(self, device_name):
        """Which of the PTZ Controls dock's controls (DOCK_CONTROLS) are enabled
        with the device selected in its camera list, by name"""
        self.select_in_dock(device_name)
        return {name: self.ui.get1(OBS_MAIN, PTZ_DOCK, name, name="enabled") for name in DOCK_CONTROLS}

    def wait_for_dock_controls(self, device_name, out_file, predicate, timeout=5, interval=0.2):
        """Polls dock_controls() until predicate(enabled) is true. out_file is
        not used: it was where the harness wrote them."""
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            last = self.dock_controls(device_name)
            if predicate(last):
                return last
            time.sleep(interval)
        raise AssertionError(f"dock controls never matched predicate; last seen: {last}")

    def set_websocket_access(self, value):
        """Chooses what a client of obs-websocket is let do ("off", "read-only"
        or "control"), in the PTZ settings dialog, which is opened if it isn't"""
        self._settings_window()
        self.ui.activate(SETTINGS_DIALOG, "websocketAccessComboBox", data=value)

    def remove_device(self, device):
        """Removes a device as a user does: chooses it in the PTZ settings
        dialog's list, presses Remove, and answers Yes"""
        self.need_ui_driver()
        uuid = self.device_uuid(device)
        assert uuid is not None, f"no device {device}"
        self._settings_window()
        self.ui.select(SETTINGS_DIALOG, "deviceList", value=uuid, role="uuid")
        # The question is modal: Remove doesn't return until it is answered
        self.ui.invoke(SETTINGS_DIALOG, "removePTZ", method="click", queued=True)
        question = (SETTINGS_DIALOG, {"class": "QMessageBox"})
        self.wait_for(lambda: self.ui.find(*question))
        # Its answer is the button clicked, not the dialog's result
        buttons = self.ui.find(*question, {"class": "QPushButton"}, all=True)
        yes = [b for b in buttons if b.get("text", "").replace("&", "") == "Yes"]
        assert len(yes) == 1, f"the question has no one Yes button: {buttons}"
        self.ui.invoke({"id": yes[0]["id"]}, method="click")
        self.wait_for(lambda: self.device_uuid(uuid) is None)

    def open_camera_report(self, device):
        """Opens the camera report dialog on a device as a user does: with
        "Create Camera Report" in the PTZ settings dialog's diagnostics"""
        self.open_settings_dialog(device)
        self.press_dialog_button("cameraReport")
        self.wait_for(lambda: self.ui.find(REPORT_DIALOG))

    def camera_report_dialog(self):
        """What the camera report dialog shows: its "status" line, the "report",
        its "progress" and "progress_max", which of its buttons are "enabled",
        and the "issue_url" Open Issue opens; or None if it isn't open"""
        self.need_ui_driver()
        ui = self.ui
        try:
            ui.find(REPORT_DIALOG)
        except UiDriverError as e:
            if e.code != "not_found":
                raise
            return None
        # Each is read on its own, while the report may still be coming: the
        # buttons first, as the dialog enables them last, once the rest is final
        enabled = {name: ui.get1(REPORT_DIALOG, name, name="enabled") for name in ("save", "copy", "openIssue")}
        progress = ui.get(REPORT_DIALOG, "progress", properties=["value", "maximum"])
        return {
            "status": ui.get1(REPORT_DIALOG, "status", name="text"),
            "report": ui.get1(REPORT_DIALOG, "report", name="plainText"),
            "progress": progress["value"],
            "progress_max": progress["maximum"],
            "enabled": enabled,
            "issue_url": ui.get1(REPORT_DIALOG, name="issueUrl"),
        }

    def close_camera_reports(self):
        """Closes every camera report dialog that is open"""
        if not self.ui.available():
            return
        try:
            dialogs = self.ui.find(REPORT_DIALOG, all=True)
        except UiDriverError as e:
            if e.code != "not_found":
                raise
            return
        for dialog in dialogs:
            try:
                self.ui.invoke({"id": dialog["id"]}, method="close")
            except UiDriverError as e:
                if e.code != "not_found":
                    raise

    def preset_view(self, out_file=None, select=None, add=None):
        """What the PTZ Controls dock's preset list is showing, read through the UI
        driver. First it can select a device in the camera list (`select`: a
        device id or name, or "none" to clear the selection). Then `add` presses
        the Add button ("button"), or chooses "camera" or "local" in its
        menu, or "toggle_default": the entry in it that has Add use the
        camera's store, or OBS's. The rows have each preset's
        "text", "id", "store" and "tooltip"; "add_menu" has the entries of the
        Add button's menu, "add_stores" the stores of the device, "device_names"
        the cameras, and "selected" whether one is selected. out_file is not
        used: it was where the harness wrote all this."""
        self.need_ui_driver()
        ui = self.ui
        camera_list = (OBS_MAIN, PTZ_DOCK, "deviceList")
        if select == "none":
            ui.select(*camera_list, value=None)
        elif select is not None:
            self.select_in_dock(select)

        actions = {"camera": "actionPresetAddCamera", "local": "actionPresetAddLocal",
                   "toggle_default": "actionPresetDefaultCamera", "button": "actionPresetAdd"}
        if add is not None:
            ui.invoke(OBS_MAIN, actions[add], method="trigger")
            # The name of the preset it made is left as it is: a new preset opens for
            # renaming, and Escape closes the editor
            editor = (OBS_MAIN, "presetListView", {"class": "QLineEdit"})
            try:
                self.wait_for(lambda: ui.find(*editor), timeout=2)
                ui.key(*editor, key="Escape")
            except (UiDriverError, AssertionError):
                pass

        rows, _, _ = ui.rows(OBS_MAIN, "presetListView", roles=["display", 256, "store", "toolTip"])
        devices, current, _ = ui.rows(*camera_list, roles=["display", "uuid"])
        view = {
            "found": True,
            "rows": [{"text": r["display"], "id": r["256"], "store": r["store"], "tooltip": r["toolTip"]}
                     for r in rows],
            "device_names": [{"text": d["display"]} for d in devices],
            "selected": current >= 0,
            "add_menu": [],
            "add_stores": [],
        }
        # The menu is only for a device with both stores: its button is then a split one
        add_text = ui.get1(OBS_MAIN, "actionPresetAdd", name="text")
        button = (OBS_MAIN, "presetToolbar", {"class": "QToolButton", "text": add_text})
        if ui.get1(*button, name="popupMode") == "MenuButtonPopup":
            for name in ("actionPresetAddCamera", "actionPresetAddLocal", "actionPresetDefaultCamera"):
                entry = ui.get(OBS_MAIN, name, properties=["text", "checked"])
                view["add_menu"].append({"text": entry["text"], "checked": entry["checked"]})
        if current >= 0:
            info = self.call_proc(devices[current]["uuid"], "ptz_preset_get_list", returns="data")["return"]
            stores = (info or {}).get("stores", {})
            view["add_stores"] = [{"text": store} for store in ("camera", "local") if stores.get(store)]
        return view

    def reorder_presets(self, out_file, **params):
        """Moves a preset of the dock's preset list, or only reports their
        order, via tests/ui-harness/reorder-presets-test.cpp's "reorder_presets"
        test. See there for the params: select, from, to and via. Returns its
        result, with "presets" as a list of names."""
        if out_file.exists():
            out_file.unlink()
        self.run_ui_test("reorder_presets", filename=str(out_file), **params)
        self.wait_for(out_file.exists)
        result = json.loads(out_file.read_text())
        result["presets"] = [r["text"] for r in result.get("presets", [])]
        return result

    def reorder_cameras(self, out_file, **params):
        """Moves a camera of the dock's or the settings dialog's list, or only
        reports their order, via tests/ui-harness/reorder-cameras-test.cpp's
        "reorder_cameras" test. See there for the params: in, set_order, from,
        to, via and reload. Returns its result, with the lists of cameras as lists
        of names, and "uuids" as the devices' uuids in order."""
        if out_file.exists():
            out_file.unlink()
        self.run_ui_test("reorder_cameras", filename=str(out_file), **params)
        self.wait_for(out_file.exists, timeout=30)
        result = json.loads(out_file.read_text())
        for key in ("devices", "settings_devices", "uuids"):
            result[key] = [r["text"] if isinstance(r, dict) else r for r in result.get(key, [])]
        return result

    def fire_hotkey(self, name):
        """Presses a hotkey of the dock, by name ("PTZ.Recall2"), the way a bound key
        does, with obs-websocket's TriggerHotkeyByName"""
        self.ws.call("TriggerHotkeyByName", {"hotkeyName": name})

    def hotkey_listed(self, text):
        """Whether OBS's Settings list a hotkey under `text`, as a user sees it
        (File > Settings > Hotkeys): the dialog is opened and its labels read with
        the UI driver"""
        self.need_ui_driver()
        settings = {"class": "OBSBasicSettings"}
        # The dialog is modal, so it is opened once the driver has answered
        self.ui.invoke(OBS_MAIN, "action_Settings", method="trigger", queued=True)
        try:
            self.wait_for(lambda: self.ui.find(settings))
            # OBS makes the page's rows some moments after the page is first shown
            self.ui.select(settings, "listWidget", value="Hotkeys")
            label = {"class": "OBSHotkeyLabel"}

            def rows():
                try:
                    return self.ui.find(settings, label, all=True)
                except UiDriverError as e:
                    if e.code != "not_found":
                        raise
                    return []

            self.wait_for(rows)
            return any(row.get("text") == text for row in rows())
        finally:
            try:
                self.ui.invoke(settings, method="reject")
            except UiDriverError:
                pass

    def wait_for_preset_view(self, out_file, predicate, timeout=5, interval=0.2):
        """Polls preset_view() until predicate(view) is true -- the view
        catches up with a model reset on a queued call."""
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            last = self.preset_view(out_file)
            if predicate(last):
                return last
            time.sleep(interval)
        raise AssertionError(f"preset view never matched predicate; last seen: {last}")

    def wait_for(self, predicate, timeout=5, interval=0.1):
        """Generic poll-until-true, for waiting on the effect of an
        asynchronous request like run_ui_test() -- unlike wait_for_state(),
        which is specifically for polling ptzsim's own /state endpoint,
        predicate takes no arguments and can check anything (a file's
        contents, another obs-websocket call's result, ...). A predicate
        that raises (e.g. a file that doesn't exist yet) is treated as
        "not yet" rather than failing the wait outright, but the last such
        exception is re-raised if the deadline is hit without success, so
        a genuine bug in the predicate still surfaces instead of a bare
        "timed out"."""
        deadline = time.time() + timeout
        last_exc = None
        while time.time() < deadline:
            try:
                if predicate():
                    return
            except Exception as e:
                last_exc = e
            time.sleep(interval)
        if last_exc is not None:
            raise last_exc
        raise AssertionError("condition never became true")


@pytest.fixture(scope="session")
def ptz_ports():
    return {
        "visca_tcp": free_port(),
        "visca_udp": free_udp_port(),
        "onvif_http": free_port(),
        "debug_http": free_port(),
        "rtsp": free_port(),
        "visca_tcp_flaky": free_port(),
        "unused_udp": free_udp_port(),
        "visca_udp_sony": free_udp_port(),
        "debug_http_sony": free_port(),
        "visca_tcp_birddog": free_port(),
        "visca_tcp_power": free_port(),
        "debug_http_power": free_port(),
        "visca_tcp_power_late": free_port(),
        "debug_http_power_late": free_port(),
        "visca_tcp_no_power": free_port(),
        "debug_http_no_power": free_port(),
        "visca_tcp_power_filter": free_port(),
        "debug_http_power_filter": free_port(),
    }


@pytest.fixture(scope="session")
def ptzsim_process(tmp_path_factory, ptz_ports):
    work = tmp_path_factory.mktemp("ptzsim")
    serial_paths = {
        "visca_serial": work / "visca-serial",
        "pelco": work / "pelco-serial",
    }
    cmd = [
        sys.executable, "-m", "ptzsim",
        "--host", "127.0.0.1",
        "--visca-tcp-port", str(ptz_ports["visca_tcp"]),
        "--visca-udp-port", str(ptz_ports["visca_udp"]),
        "--pelco-address", "1",
        "--onvif-http-port", str(ptz_ports["onvif_http"]),
        "--rtsp-port", str(ptz_ports["rtsp"]),
        "--debug-http-port", str(ptz_ports["debug_http"]),
    ]
    if IS_WINDOWS:  # there is no pty there to be a serial port
        cmd += ["--no-visca-serial", "--no-pelco"]
    else:
        cmd += ["--visca-serial-path", str(serial_paths["visca_serial"]),
                "--pelco-serial-path", str(serial_paths["pelco"])]
    with output_log("ptzsim") as out:
        proc = subprocess.Popen(cmd, cwd=REPO_ROOT / "scripts", stdout=out, stderr=subprocess.STDOUT)
    wait_for_port("127.0.0.1", ptz_ports["debug_http"], timeout=15)
    yield {"proc": proc, "ports": ptz_ports, "serial_paths": serial_paths}
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()


@pytest.fixture(scope="session")
def sony_ptzsim(ptz_ports):
    """A VISCA-over-IP-only ptzsim that behaves like a real Sony camera
    (--visca-udp-sony-quirks), on its own ports, wired to DEVICE_IDS
    ["visca-udp-sony"]. Session-scoped, and started before OBS (see
    obs_world) so the device's startup handshake is against a camera that
    is there. Its counters are read back with stats(), of which a test
    should take a difference: they count everything since it started."""
    cmd = [
        sys.executable, "-m", "ptzsim",
        "--host", "127.0.0.1",
        "--visca-udp-port", str(ptz_ports["visca_udp_sony"]),
        "--visca-udp-sony-quirks",
        "--no-visca-tcp", "--no-visca-serial", "--no-onvif", "--no-pelco",
        "--debug-http-port", str(ptz_ports["debug_http_sony"]),
    ]
    with output_log("ptzsim-sony") as out:
        proc = subprocess.Popen(cmd, cwd=REPO_ROOT / "scripts", stdout=out, stderr=subprocess.STDOUT)
    wait_for_port("127.0.0.1", ptz_ports["debug_http_sony"], timeout=15)

    class SonySim:
        def stats(self):
            url = f"http://127.0.0.1:{ptz_ports['debug_http_sony']}/state"
            with urllib.request.urlopen(url, timeout=2) as resp:
                return json.load(resp)["visca_udp_quirks"]

        def hold_next_inquiry(self):
            """Has the camera answer the next inquiry only once a different
            request has come after it"""
            url = f"http://127.0.0.1:{ptz_ports['debug_http_sony']}/visca_udp/hold_next_inquiry"
            request = urllib.request.Request(url, data=b"", method="POST")
            with urllib.request.urlopen(request, timeout=2):
                pass

    yield SonySim()
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()


@pytest.fixture(scope="session")
def birddog_ptzsim(ptz_ports):
    """A VISCA-over-TCP-only ptzsim that, like a BirdDog, answers the block
    inquiries with a syntax error (--visca-no-block-inquiries), and the
    version inquiry too, so the plugin can't tell what it is and has to
    find out what it has, wired to DEVICE_IDS["visca-tcp-birddog"]. Like a
    BirdDog it ACKs a command and never completes it (--visca-no-completions),
    so the device can't tell when a move has ended. Started before OBS (see
    obs_world)."""
    cmd = [
        sys.executable, "-m", "ptzsim",
        "--host", "127.0.0.1",
        "--visca-tcp-port", str(ptz_ports["visca_tcp_birddog"]),
        "--visca-no-block-inquiries", "--visca-no-version-inquiry", "--visca-no-completions",
        "--no-visca-udp", "--no-visca-serial", "--no-onvif", "--no-pelco",
    ]
    with output_log("ptzsim-birddog") as out:
        proc = subprocess.Popen(cmd, cwd=REPO_ROOT / "scripts", stdout=out, stderr=subprocess.STDOUT)
    wait_for_port("127.0.0.1", ptz_ports["visca_tcp_birddog"], timeout=15)
    yield proc
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()


class StandbyPtzsim:
    """A VISCA-over-TCP-only ptzsim that starts with the camera powered off,
    on its own ports, that reports whether it is on. start() and stop() it
    to have the camera there or not."""

    def __init__(self, tcp_port, debug_port):
        self.tcp_port = tcp_port
        self.debug_port = debug_port
        self.proc = None

    def start(self):
        cmd = [
            sys.executable, "-m", "ptzsim",
            "--host", "127.0.0.1",
            "--visca-tcp-port", str(self.tcp_port),
            "--no-visca-udp", "--no-visca-serial", "--no-onvif", "--no-pelco",
            "--start-in-standby",
            "--debug-http-port", str(self.debug_port),
        ]
        with output_log("ptzsim-standby") as out:
            self.proc = subprocess.Popen(cmd, cwd=REPO_ROOT / "scripts", stdout=out, stderr=subprocess.STDOUT)
        wait_for_port("127.0.0.1", self.debug_port, timeout=15)
        wait_for_port("127.0.0.1", self.tcp_port, timeout=15)

    def stop(self):
        if self.proc is None:
            return
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.proc = None

    def power(self):
        url = f"http://127.0.0.1:{self.debug_port}/state"
        with urllib.request.urlopen(url, timeout=2) as resp:
            return json.load(resp)["power"]


@pytest.fixture(scope="session")
def power_ptzsim(ptz_ports):
    """The camera DEVICE_IDS["visca-tcp-power"] talks to, powered off until
    OBS turns it on. Started before OBS (see obs_world), and not stopped
    until after it has exited, to see if it was turned off again."""
    sim = StandbyPtzsim(ptz_ports["visca_tcp_power"], ptz_ports["debug_http_power"])
    sim.start()
    yield sim
    sim.stop()


@pytest.fixture(scope="session")
def filter_power_ptzsim(ptz_ports):
    """The camera a test's filter-owned device is pointed at (it is not
    configured in advance, as the others are), powered off until then. Like
    power_ptzsim, there before OBS starts and stopped after it has exited."""
    sim = StandbyPtzsim(ptz_ports["visca_tcp_power_filter"], ptz_ports["debug_http_power_filter"])
    sim.start()
    yield sim
    sim.stop()


@pytest.fixture(scope="session")
def late_power_ptzsim(ptz_ports):
    """The camera DEVICE_IDS["visca-tcp-power-late"] talks to, which is not
    there while OBS starts: obs_world starts it as soon as OBS has finished
    loading, which it says by no longer being "not ready" to obs-websocket."""
    sim = StandbyPtzsim(ptz_ports["visca_tcp_power_late"], ptz_ports["debug_http_power_late"])
    yield sim
    sim.stop()


@pytest.fixture(scope="session")
def no_power_ptzsim(ptz_ports):
    """The camera DEVICE_IDS["visca-tcp-no-power"], which has neither power
    setting, talks to: powered off, and to be left as it is. A test that
    turns it on sets `left_on`, for whether closing OBS turns it off again."""
    sim = StandbyPtzsim(ptz_ports["visca_tcp_no_power"], ptz_ports["debug_http_no_power"])
    sim.left_on = False
    sim.start()
    yield sim
    sim.stop()


class FlakyPtzsim:
    """A dedicated, VISCA-TCP-only ptzsim instance on its own fixed port
    (ptz_ports["visca_tcp_flaky"], wired to device_name
    DEVICE_IDS["visca-tcp-flaky"] by write_ptz_plugin_config()) that a
    test can freely stop() and start() again -- unlike ptzsim_process
    (session-scoped and shared by every other test in this suite),
    killing this one doesn't disturb any other device.

    obs-ptz's own automatic reconnect (PTZViscaOverTCP::
    on_socket_stateChanged() retries connectToHost() every 1.9s, see
    src/drivers/visca/ptz-visca-tcp.cpp) picks a freshly (re)started listener back up
    on its own; nothing here needs to poke the plugin to make that
    happen."""

    def __init__(self, port):
        self.port = port
        self.proc = None

    def start(self):
        if self.proc is not None:
            return
        cmd = [
            sys.executable, "-m", "ptzsim",
            "--host", "127.0.0.1",
            "--visca-tcp-port", str(self.port),
            "--no-visca-udp", "--no-visca-serial", "--no-onvif", "--no-pelco",
        ]
        with output_log("ptzsim-flaky") as out:
            self.proc = subprocess.Popen(cmd, cwd=REPO_ROOT / "scripts", stdout=out, stderr=subprocess.STDOUT)
        wait_for_port("127.0.0.1", self.port, timeout=10)

    def stop(self):
        if self.proc is None:
            return
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.proc = None


@pytest.fixture
def flaky_ptzsim(obs_world, ptz_ports):
    sim = FlakyPtzsim(ptz_ports["visca_tcp_flaky"])
    sim.start()
    yield sim
    sim.stop()


def windows_obs_log(home: Path):
    """The file the OBS on this home is logging to. OBS on Windows says nothing
    on its standard output but a few lines as it starts: what it logs is in the
    file it keeps itself"""
    logs = sorted((obs_config_root(home) / "logs").glob("*.txt"), key=lambda p: p.stat().st_mtime)
    return logs[-1] if logs else None


def connect_when_ready(ws_port, ws_password):
    """A connection to the OBS listening on ws_port, once its frontend has
    loaded"""
    deadline = time.time() + 30
    last_error = None
    ws = None
    while time.time() < deadline:
        try:
            ws = Client(f"ws://127.0.0.1:{ws_port}", password=ws_password)
            break
        except (OSError, ObsWebSocketError) as e:
            last_error = e
            time.sleep(1)
    if ws is None:
        raise RuntimeError(f"could not complete obs-websocket handshake: {last_error}")

    # The obs-websocket server thread starts (and completes the
    # handshake above) before OBS's frontend has finished loading its
    # scene collection. Requests made in that window are rejected with
    # RequestStatus::NotReady (code 207) even though the connection is
    # already up, so poll a harmless read-only request until the
    # frontend catches up.
    deadline = time.time() + 30
    last_error = None
    while time.time() < deadline:
        try:
            ws.call("GetSceneList")
            return ws
        except ObsWebSocketError as e:
            if e.status.get("code") != OBS_NOT_READY:
                raise
            last_error = e
            time.sleep(0.2)
    raise RuntimeError(f"OBS frontend never became ready: {last_error}")


def first_run_leaks(obs_binary, tmp_path_factory):
    """How many allocations OBS says it leaked as it quit, after a first run on
    a fresh profile with neither the plugin nor the UI driver loaded: what OBS
    leaks by itself, which is not always nothing (one, from OBS 32.2.2 on
    Windows, whatever plugins it has). They are turned off in its plugin
    manager's list of modules, which OBS before 32.0 doesn't have: there, this
    counts the plugin's leaks too.

    On Windows this is the folder the session's own OBS keeps its profile in,
    so it is run before that is made, and its profile is gone afterwards."""
    home = portable_obs_home(obs_binary) if IS_WINDOWS else tmp_path_factory.mktemp("obs-first-run-home")
    ws_port, ws_password = free_port(), "ptzsim-ci-password"
    write_websocket_config(home, ws_port, ws_password)
    module = {"display_name": "", "id": "", "version": "", "enabled": False, "sources": [], "outputs": [],
              "encoders": [], "services": []}
    modules = obs_config_root(home) / "plugin_manager" / "modules.json"
    modules.parent.mkdir(parents=True, exist_ok=True)
    modules.write_text(json.dumps([dict(module, module_name=name) for name in ("obs-ptz", "obs-ui-driver")]))

    env = dict(os.environ)
    env["HOME"] = str(home)
    env["OBS_WEBSOCKET_SERVER_ENABLE"] = "true"
    env["OBS_WEBSOCKET_SERVER_PORT"] = str(ws_port)
    env["OBS_WEBSOCKET_SERVER_PASSWORD"] = ws_password
    if platform.system() == "Darwin":
        isolate_macos_obs(home, env)
    xvfb = None
    if platform.system() == "Linux" and not env.get("DISPLAY") and shutil.which("Xvfb"):
        xvfb, env["DISPLAY"] = start_xvfb()
    try:
        with output_log("obs-first-run") as out:
            log = out.path
            proc = subprocess.Popen([obs_binary, "--disable-updater"],
                                    cwd=Path(obs_binary).parent if IS_WINDOWS else home, env=env, stdout=out,
                                    stderr=subprocess.STDOUT, **({} if IS_WINDOWS else {"start_new_session": True}))
        try:
            wait_for_port("127.0.0.1", ws_port, timeout=90)
            if IS_WINDOWS:
                log = windows_obs_log(home) or log
            connect_when_ready(ws_port, ws_password).close()
        finally:
            exited = stop_process_group(proc, timeout=10, sig=signal.SIGINT)
    finally:
        if xvfb is not None:
            stop_process_group(xvfb, timeout=5)
    assert exited, f"OBS without the plugin did not quit by itself: see {log}"
    reported = re.findall(r"Number of memory leaks: (\d+)", log.read_text(errors="replace"))
    assert reported, f"OBS without the plugin did not say how much it leaked: see {log}"
    if IS_WINDOWS:
        # The session's OBS has a fresh one, and config.prev stays the last run's
        shutil.rmtree(home / "config")
    return int(reported[0])


def isolate_macos_obs(home: Path, env):
    """Point OBS on macOS at home instead of the real user's.

    OBS finds its config directory through NSSearchPathForDirectoriesInDomains,
    which asks the system for the account's home directory rather than reading
    $HOME, so setting $HOME isolates nothing and a test run would load, and
    change, the real profile. Core Foundation does honor CFFIXED_USER_HOME,
    which gives OBS an empty profile here, as $HOME does on Linux. So:

    - the plugin is not in this home's plugins/, where OBS now looks for it:
      PTZSIM_PLUGIN_BUNDLE is the obs-ptz.plugin to test, else the one
      installed for the real user. PTZ_UI_DRIVER_BUNDLE, if it is set and
      there, is the UI driver (tests/ui-driver/) to load with it;
    - OBS opens its first-run wizard, a modal dialog, unless it has run before
      (user.ini's FirstRun), and another one asking for permissions it will
      never be given, unless it has shown that already (global.ini's
      MacOSPermissionsDialogLastShown, which is a dialog version, so set it
      high enough to cover any a later OBS has).
    """
    env["CFFIXED_USER_HOME"] = str(home)

    bundle = os.environ.get("PTZSIM_PLUGIN_BUNDLE")
    if not bundle:
        import pwd
        bundle = Path(pwd.getpwuid(os.getuid()).pw_dir) / "Library" / "Application Support" / "obs-studio" / \
            "plugins" / "obs-ptz.plugin"
    if not Path(bundle).is_dir():
        raise RuntimeError(f"no plugin to test at {bundle}: set PTZSIM_PLUGIN_BUNDLE to the obs-ptz.plugin to use")
    plugins = obs_config_root(home) / "plugins"
    plugins.mkdir(parents=True, exist_ok=True)
    (plugins / "obs-ptz.plugin").symlink_to(Path(bundle).resolve())
    driver = os.environ.get("PTZ_UI_DRIVER_BUNDLE")
    if driver and Path(driver).is_dir():
        (plugins / "obs-ui-driver.plugin").symlink_to(Path(driver).resolve())

    (obs_config_root(home) / "user.ini").write_text("[General]\nFirstRun=true\nLanguage=en-GB\n")
    with open(obs_config_root(home) / "global.ini", "a") as f:
        f.write("\n[General]\nMacOSPermissionsDialogLastShown=1000\n")


@pytest.fixture(scope="session")
def obs_world(request, tmp_path_factory, ptzsim_process, sony_ptzsim, birddog_ptzsim, power_ptzsim,
              filter_power_ptzsim, late_power_ptzsim, no_power_ptzsim):
    obs_binary = os.environ.get(
        "PTZSIM_OBS_BINARY",
        "/Applications/OBS.app/Contents/MacOS/OBS" if platform.system() == "Darwin" else "obs")
    # Only when a test compares with it, as it is another OBS to start and
    # quit; and before this session's own OBS, see first_run_leaks()
    first_run = None
    if any("obs_first_run_leaks" in item.fixturenames for item in request.session.items):
        first_run = first_run_leaks(obs_binary, tmp_path_factory)

    home = tmp_path_factory.mktemp("obs-home")
    if IS_WINDOWS:
        home = portable_obs_home(obs_binary)
    write_ptz_plugin_config(home, ptzsim_process["ports"], ptzsim_process["serial_paths"])

    ws_port = free_port()
    ws_password = "ptzsim-ci-password"
    write_websocket_config(home, ws_port, ws_password)

    obs_cwd = home

    if IS_WINDOWS:
        # OBS finds its data folder from where it is run, not from where it is
        obs_cwd = Path(obs_binary).parent
    env = dict(os.environ)
    env["HOME"] = str(home)
    env["OBS_WEBSOCKET_SERVER_ENABLE"] = "true"
    env["OBS_WEBSOCKET_SERVER_PORT"] = str(ws_port)
    env["OBS_WEBSOCKET_SERVER_PASSWORD"] = ws_password
    # Activates tests/ui-harness/ (test_preset_import_export.py) when the
    # plugin was built with -DENABLE_UI_TESTS=ON; a harmless no-op
    # otherwise (ptz_load_ui_tests() is a stub in that case -- see
    # src/ptz.h), so always setting it keeps this fixture usable either way.
    env["PTZ_UI_TEST_HARNESS"] = "1"
    # And the UI driver (tests/ui-driver/), a plugin of its own, which is only
    # there when the scripts that run this suite installed it
    env["OBS_UI_DRIVER"] = "1"
    if platform.system() == "Darwin":
        isolate_macos_obs(home, env)

    # With no display, start an Xvfb here rather than through xvfb-run: a
    # SIGTERM to xvfb-run ends xvfb-run and leaves OBS and its Xvfb running.
    # OBS then never closes (so never turns cameras off), and the next run's
    # OBS can't start next to it. Launched directly, proc is OBS itself.
    xvfb = None
    if platform.system() == "Linux" and not env.get("DISPLAY") and shutil.which("Xvfb"):
        xvfb, env["DISPLAY"] = start_xvfb()

    # "log" is the file OBS's output goes to, which stays after it has quit
    running = {"proc": None, "ws": None, "events": None, "signals": None, "log": None}

    def start_obs():
        """Starts OBS on this home and connects to it once it has loaded"""
        # An OBS that has quit leaves the simulated serial ports in the
        # exclusive use it asked for, as it doesn't close them on its way out:
        # a real port's goes with the last program that has it open, which the
        # simulator, keeping its own end open, never is
        if not IS_WINDOWS:
            release = urllib.request.Request(
                f"http://127.0.0.1:{ptzsim_process['ports']['debug_http']}/serial/release", data=b"",
                method="POST")
            with urllib.request.urlopen(release, timeout=5):
                pass
        # Its own process group, so that whatever OBS starts goes when it does.
        with output_log("obs") as out:
            running["log"] = out.path
            running["proc"] = subprocess.Popen([obs_binary, "--disable-updater"], cwd=obs_cwd, env=env, stdout=out,
                                               stderr=subprocess.STDOUT,
                                               **({} if IS_WINDOWS else {"start_new_session": True}))
        wait_for_port("127.0.0.1", ws_port, timeout=90)
        if IS_WINDOWS:
            running["log"] = windows_obs_log(home) or running["log"]
        running["ws"] = ws = connect_when_ready(ws_port, ws_password)

        # obs-websocket's SetCurrentProgramScene doesn't wait for the
        # switch to finish -- with the default (animated) transition, a
        # scene switch issued while the previous one is still transitioning
        # can silently fail to activate the new scene's sources at all, so
        # a ptz_action_source never fires. Force an instant Cut so each
        # trigger_action's scene switch always completes immediately.
        ws.call("SetCurrentSceneTransition", {"transitionName": "Cut"})
        for connection in ("events", "signals"):
            running[connection] = Client(f"ws://127.0.0.1:{ws_port}", password=ws_password,
                                         event_subscriptions=EVENT_VENDORS)

    def stop_obs():
        """Quits OBS the way a user does, and says whether it got to exit by itself"""
        for connection in ("ws", "events", "signals"):
            if running[connection] is not None:
                running[connection].close()
                running[connection] = None
        # SIGINT, not SIGTERM: OBS closes its main window -- a normal quit --
        # on SIGINT since 23.2, but only on SIGTERM since 32.1. Before that
        # a SIGTERM just kills it, and nothing gets turned off.
        return stop_process_group(running["proc"], timeout=10, sig=signal.SIGINT)

    def restart_obs():
        """Quits OBS and starts it again on the same profile, as a user restarting it would,
        and gives the world the new connection. Returns the file the output of the OBS
        that quit is in"""
        quit_log = running["log"]
        quitting = running["proc"]
        assert stop_obs(), "OBS did not quit by itself"
        # What it exited with: not 0 if it crashed on the way out
        world.exit_code = quitting.returncode
        start_obs()
        # Not left to quit again before it has settled: a device set to turn its
        # camera on when OBS starts has done so, as the check that it turns it
        # off as OBS closes, at the end of the session, takes for granted
        world.wait_for(lambda: power_ptzsim.power() is True, timeout=30)
        world.ws = running["ws"]
        world.events = running["events"]
        world.signals = running["signals"]
        return quit_log

    try:
        start_obs()

        # OBS has finished loading (obs-websocket is not ready before), and
        # that was the time devices were told to power their cameras on: this
        # one was not there to be told
        late_power_ptzsim.start()

        world = World(running["ws"], f"http://127.0.0.1:{ptzsim_process['ports']['debug_http']}/state", DEVICE_NAMES)
        world.events = running["events"]
        world.signals = running["signals"]
        world.ws_url, world.ws_password = f"ws://127.0.0.1:{ws_port}", ws_password
        world.config_dir = obs_config_root(home) / "plugin_config" / "obs-ptz"
        world.restart_obs = restart_obs
        world.obs_log = lambda: running["log"]
        world.first_run_leaks = first_run
        # Settled before a test can end the session, as restart_obs() does for
        # a restart: an OBS that quits before its camera has been turned on
        # sends the on and the off together, and the on can land last
        world.wait_for(lambda: power_ptzsim.power() is True, timeout=30)
        yield world
    finally:
        exited_cleanly = stop_obs() if running["proc"] is not None else False
        if xvfb is not None:
            stop_process_group(xvfb, timeout=5)
    # A device set to turn its camera off when OBS closes has done so by now.
    # Nothing else can see it happen: OBS is gone. Not if OBS had to be killed
    # (it can hang on the way out on macOS), which is not what is being looked
    # at.
    if exited_cleanly:
        assert power_ptzsim.power() is False, "OBS closed without turning the camera off"
        assert filter_power_ptzsim.power() is False, "OBS closed without turning the filter's camera off"
        if no_power_ptzsim.left_on:
            assert no_power_ptzsim.power() is True, "OBS closed and turned off a camera it was not set to"


@pytest.fixture(scope="session")
def obs_first_run_leaks(obs_world):
    """What OBS leaks by itself on a first run, see first_run_leaks(): obs_world
    finds it before its own OBS starts, when a test asks for this"""
    return obs_world.first_run_leaks


@pytest.fixture(autouse=True)
def _cleanup_scenes_between_tests(obs_world):
    yield
    obs_world.cleanup_scenes()
