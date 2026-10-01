"""Fixtures that boot one ptzsim instance and one real OBS Studio process
(plugin built from this checkout) wired together, for the whole test
session.

Device wiring is entirely config-file driven: ptzsim's cameras are
started on fixed local ports, and obs-ptz's own config.json (the file
PTZControls::LoadConfig() reads -- see src/ptz-controls.cpp) is
pre-written with one device per ptzsim backend, each given an explicit
"id" so device_id is known ahead of time. OBS itself is driven at
runtime over obs-websocket (bundled since OBS 28), which every test
module uses to add a `ptz_action_source` (src/ptz-action-source.c) to a
scene and make that scene current -- which is what fires the source's
configured action against the real PTZDevice, exercising the exact same
code path a user's joystick/hotkey would.
"""

import json
import os
import platform
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
from obsws import Client, ObsWebSocketError

REPO_ROOT = Path(__file__).resolve().parents[2]

# obs-websocket RequestStatus::NotReady -- returned while OBS's frontend
# is still starting up, even after the websocket handshake has completed.
OBS_NOT_READY = 207

# Device ids are fixed so tests can address them without querying OBS for
# a device list (obs-ptz doesn't expose one over obs-websocket).
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
    # Devices for test_device_source_binding.py. Each is configured with
    # the name of an OBS source that doesn't exist when OBS starts (the
    # config is loaded before OBS loads its scene collection), which the
    # test then creates, renames and removes. One device per test, since
    # a device's binding state carries over from one test to the next.
    "source-late": 7,
    "source-rename": 8,
    "source-recreate": 9,
    "source-held": 10,
    "source-held-replaced": 11,
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


def write_preset_file(path, presets, preset_max=16, device="unused"):
    """Writes a preset export file in the shape PTZDevice::exportPresets()
    produces (see on_actionPresetExport_triggered() in
    src/ptz-controls.cpp), for test_preset_import_export.py to feed to
    the real "Import Presets..." action via World.run_ui_test()."""
    path.write_text(json.dumps({
        "obs-ptz-preset-format": 1,
        "device": device,
        "preset_max": preset_max,
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
    if platform.system() == "Darwin":
        return home / "Library" / "Application Support" / "obs-studio"
    return home / ".config" / "obs-studio"


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
    # Nothing listens on these devices' UDP port: they only exist to be
    # bound to sources, not to talk to a camera.
    for key in ("source-late", "source-rename", "source-recreate", "source-held", "source-held-replaced"):
        devices.append({
            "id": DEVICE_IDS[key],
            "name": f"sim-{key}",
            "type": "visca-over-ip",
            "host": "127.0.0.1",
            "udp_port": ports["unused_udp"],
        })
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
        **json.loads((REPO_ROOT / "src" / "visca-profiles" / "birddog-p100.json").read_text()),
        "models": ["0109:2020", "0123:0004"],
        "remove": ["tally_preview", "low_latency"],
    },
    # One that only changes a control: that the AE mode can't be set
    "control-only.json": {
        "id": "test-control-only",
        "models": ["0123:0005"],
        "controls": [{"key": "ae_mode", "set": None}],
    },
    # One with a new state value that isn't named "user_...", which only a
    # command set shipped with the plugin can have: left out
    "unprefixed.json": {
        "id": "test-unprefixed",
        "models": ["0123:0006"],
        "controls": [{"key": "auto_tracking",
                      "reads": [{"cmd": "81090a11ff", "results": [{"type": "flag", "offset": 2}]}]}],
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
    return os.fdopen(fd, "w")


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


def stop_process_group(proc, timeout, sig=signal.SIGTERM):
    """Sends sig to proc (started with start_new_session=True) and waits for
    it to exit, then ends whatever else is left in its process group.
    Returns whether proc exited on its own; SIGKILLs it if not."""
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
    def __init__(self, ws, debug_url, device_ids):
        self.ws = ws
        self.debug_url = debug_url
        self.device_ids = device_ids
        self._scene_counter = 0

    def state(self):
        with urllib.request.urlopen(self.debug_url, timeout=5) as resp:
            return json.loads(resp.read())

    def wait_for_state(self, predicate, timeout=5, interval=0.1):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            last = self.state()
            if predicate(last):
                return last
            time.sleep(interval)
        raise AssertionError(f"state never matched predicate; last seen: {last}")

    def trigger_action(self, device_id, action, preset_id=0, pan_speed=0.0, tilt_speed=0.0):
        """Create a ptz_action_source in a fresh scene and make that scene
        current, which fires the action via PTZ_ACTION_TRIGGER_PROGRAM_ACTIVE
        (the source's default trigger -- see ptz_action_source_activate())."""
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
                "device_id": device_id,
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

    def _query_device_source(self, out_file, **params):
        if out_file.exists():
            out_file.unlink()
        self.run_ui_test("get_device_source", filename=str(out_file), **params)
        self.wait_for(out_file.exists)
        return json.loads(out_file.read_text())

    def device_source(self, device_id, out_file):
        """Fetches which OBS source device_id is bound to, and the
        device's name, via tests/ui-harness/device-source-test.cpp's
        "get_device_source" test. Like device_status(), removes any stale
        out_file first and waits for the rewritten one. Note that asking
        can itself bind the device to a source that has just appeared."""
        return self._query_device_source(out_file, device_id=device_id)

    def device_by_name(self, name, out_file):
        """device_source() for a device whose id isn't known ahead of
        time, such as one an OBS filter created, found by its name.
        Nothing but {"found": False} is there if there's no such device."""
        return self._query_device_source(out_file, name=name)

    def wait_for_device_source(self, device_id, out_file, predicate, timeout=5, interval=0.2):
        """Polls device_source() until predicate(result) is true -- name
        changes and the device list's cached live/locked state reach it
        asynchronously, via signals queued onto the GUI thread."""
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            last = self.device_source(device_id, out_file)
            if predicate(last):
                return last
            time.sleep(interval)
        raise AssertionError(f"device {device_id} source never matched predicate; last seen: {last}")

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

    def saved_devices(self, out_file):
        """The device configs the plugin would save to its config file
        (tests/ui-harness/device-source-test.cpp's "get_saved_devices")."""
        if out_file.exists():
            out_file.unlink()
        self.run_ui_test("get_saved_devices", filename=str(out_file))
        self.wait_for(out_file.exists)
        return json.loads(out_file.read_text())["devices"]

    def hold_source(self, name, out_file, release=False):
        """Takes (or, with release=True, drops) a strong reference to the
        source called `name`, through tests/ui-harness/device-source-test.
        cpp's "hold_source" test, and waits for it to have done so -- the
        request is queued onto the GUI thread, so a caller that goes on to
        remove the source over obs-websocket would otherwise race it."""
        if out_file.exists():
            out_file.unlink()
        self.run_ui_test("hold_source", name=name, release="1" if release else "0", filename=str(out_file))
        self.wait_for(out_file.exists)
        assert json.loads(out_file.read_text())["ok"] is True

    def cleanup_scenes(self):
        for i in range(1, self._scene_counter + 1):
            try:
                self.ws.call("RemoveScene", {"sceneName": f"ptzsim-test-{i}"})
            except ObsWebSocketError:
                pass

    def run_ui_test(self, cmd, **params):
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
        it wrote) have to poll for it, e.g. with wait_for() below."""
        response = self.ws.call("CallVendorRequest", {
            "vendorName": "obs-ptz",
            "requestType": "ui_test_run",
            "requestData": {"cmd": cmd, **{k: str(v) for k, v in params.items()}},
        })
        return response["responseData"]

    def export_and_wait(self, device_id, out_file, expected_presets, timeout=5, interval=0.1):
        """Triggers the real "Export Presets..." action once (via
        run_ui_test()) for device_id, writing to out_file, then waits for
        it to show expected_presets -- run_ui_test()'s dispatch is queued
        onto the GUI thread, not synchronous, so the file may not exist
        yet the instant this returns. Returns the parsed file."""
        self.run_ui_test("export_presets", device_id=device_id, filename=str(out_file))

        def matches():
            return out_file.exists() and json.loads(out_file.read_text()).get("presets") == expected_presets

        self.wait_for(matches, timeout=timeout, interval=interval)
        return json.loads(out_file.read_text())

    def device_settings(self, device_id, out_file):
        """Fetches, as three sets of key names, what device_id's settings
        properties edit ("property_keys"), what saving it writes
        ("save_keys"), and what its PTZ filter (if it has one) would
        persist ("filter_keys"), via tests/ui-harness/device-settings-test.cpp's
        "get_device_settings" test. Also returns what saving it wrote, with
        its values, as "saved", and what each string list property offers,
        as "lists": {key: [value, ...]}."""
        if out_file.exists():
            out_file.unlink()
        self.run_ui_test("get_device_settings", device_id=device_id, filename=str(out_file))
        self.wait_for(out_file.exists)
        raw = json.loads(out_file.read_text())
        keys = {name: {e["key"] for e in raw.get(name, [])} for name in ("property_keys", "save_keys", "filter_keys")}
        keys["saved"] = raw.get("saved", {})
        keys["lists"] = {e["key"]: [v["value"] for v in e["values"]] for e in raw.get("lists", [])}
        return keys

    def camera_report(self, device_id, out_file):
        """Fetches device_id's camera report, or None if it has none, via
        tests/ui-harness/device-state-test.cpp's "get_camera_report" test"""
        if out_file.exists():
            out_file.unlink()
        self.run_ui_test("get_camera_report", device_id=device_id, filename=str(out_file))
        self.wait_for(out_file.exists)
        return json.loads(out_file.read_text()).get("report")

    def device_state(self, device_id, out_file):
        """Fetches device_id's whole transient state ("state"), via
        tests/ui-harness/device-state-test.cpp's "get_device_state" test.
        Like device_status(), removes any stale out_file first."""
        if out_file.exists():
            out_file.unlink()
        self.run_ui_test("get_device_state", device_id=device_id, filename=str(out_file))
        self.wait_for(out_file.exists)
        return json.loads(out_file.read_text())

    def wait_for_device_state(self, device_id, out_file, predicate, timeout=5, interval=0.2):
        """Polls device_state() until predicate(result) is true."""
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            last = self.device_state(device_id, out_file)
            if predicate(last):
                return last
            time.sleep(interval)
        raise AssertionError(f"device {device_id} state never matched predicate; last seen: {last}")

    def device_signals(self, device_id, out_file):
        """Fetches how often PTZListModel's deviceSettingsUpdated() and
        deviceStateUpdated() fired for device_id since watch_device_signals(),
        and which state keys the latter reported as changed, via
        tests/ui-harness/device-signals-test.cpp's "get_device_signals"
        test."""
        if out_file.exists():
            out_file.unlink()
        self.run_ui_test("get_device_signals", device_id=device_id, filename=str(out_file))
        self.wait_for(out_file.exists)
        raw = json.loads(out_file.read_text())
        raw["state_keys"] = {e["key"] for e in raw.get("state_keys", [])}
        return raw

    def wait_for_device_signals(self, device_id, out_file, predicate, timeout=5, interval=0.2):
        """Polls device_signals() until predicate(result) is true."""
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            last = self.device_signals(device_id, out_file)
            if predicate(last):
                return last
            time.sleep(interval)
        raise AssertionError(f"device {device_id} signals never matched predicate; last seen: {last}")

    def settings_dialog(self, out_file):
        """Fetches what the open PTZ settings dialog's two views hold, via
        tests/ui-harness/settings-dialog-test.cpp's "get_settings_dialog"
        test (open it first, with run_ui_test("open_settings_dialog"))."""
        if out_file.exists():
            out_file.unlink()
        self.run_ui_test("get_settings_dialog", filename=str(out_file))
        self.wait_for(out_file.exists)
        raw = json.loads(out_file.read_text())
        for name in ("settings_keys", "state_keys"):
            raw[name] = {e["key"] for e in raw.get(name, [])}
        return raw

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

    def device_status(self, device_id, out_file):
        """Fetches device_id's live {"connected"} status via
        tests/ui-harness/device-status-test.cpp's "get_device_status"
        test -- the only way to observe PTZDevice::isConnected() from
        outside the plugin, since obs-websocket has no device list or
        property read of its own. Removes any stale out_file first so a
        slow dispatch can never be mistaken for a fresh read, then waits
        for the (re)written file before parsing it."""
        if out_file.exists():
            out_file.unlink()
        self.run_ui_test("get_device_status", device_id=device_id, filename=str(out_file))
        self.wait_for(out_file.exists)
        return json.loads(out_file.read_text())

    def wait_for_device_status(self, device_id, out_file, predicate, timeout=5, interval=0.2):
        """Polls device_status() until predicate(status) is true,
        re-dispatching get_device_status each time so out_file always
        reflects a fresh read rather than one cached from an earlier
        call."""
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            last = self.device_status(device_id, out_file)
            if predicate(last):
                return last
            time.sleep(interval)
        raise AssertionError(f"device {device_id} status never matched predicate; last seen: {last}")

    def dock_controls(self, device_id, out_file):
        """Which of the PTZ Controls dock's controls are enabled with the
        device selected, via tests/ui-harness/dock-controls-test.cpp's
        "get_dock_controls" test"""
        if out_file.exists():
            out_file.unlink()
        self.run_ui_test("get_dock_controls", device_id=device_id, filename=str(out_file))
        self.wait_for(out_file.exists)
        return json.loads(out_file.read_text())

    def wait_for_dock_controls(self, device_id, out_file, predicate, timeout=5, interval=0.2):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            last = self.dock_controls(device_id, out_file)
            if predicate(last["enabled"]):
                return last["enabled"]
            time.sleep(interval)
        raise AssertionError(f"dock controls never matched predicate; last seen: {last}")

    def preset_view(self, out_file, select=None, add_device=None, remove_device=None):
        """What the PTZ Controls dock's preset list is showing, via
        tests/ui-harness/preset-view-test.cpp's "get_preset_view" test.
        First, in this order, it can select a device in the camera list
        (`select`: a device id, or "none" to clear the selection), add a
        device with a given name (which resets the model), or remove a
        device by name."""
        if out_file.exists():
            out_file.unlink()
        params = {k: v for k, v in (("select", select), ("add_device", add_device),
                                    ("remove_device", remove_device)) if v is not None}
        self.run_ui_test("get_preset_view", filename=str(out_file), **params)
        self.wait_for(out_file.exists)
        return json.loads(out_file.read_text())

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
        "--visca-serial-path", str(serial_paths["visca_serial"]),
        "--pelco-serial-path", str(serial_paths["pelco"]),
        "--pelco-address", "1",
        "--onvif-http-port", str(ptz_ports["onvif_http"]),
        "--rtsp-port", str(ptz_ports["rtsp"]),
        "--debug-http-port", str(ptz_ports["debug_http"]),
    ]
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
    find out what it has, wired to DEVICE_IDS["visca-tcp-birddog"]. Started
    before OBS (see obs_world)."""
    cmd = [
        sys.executable, "-m", "ptzsim",
        "--host", "127.0.0.1",
        "--visca-tcp-port", str(ptz_ports["visca_tcp_birddog"]),
        "--visca-no-block-inquiries", "--visca-no-version-inquiry",
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
    (ptz_ports["visca_tcp_flaky"], wired to device_id
    DEVICE_IDS["visca-tcp-flaky"] by write_ptz_plugin_config()) that a
    test can freely stop() and start() again -- unlike ptzsim_process
    (session-scoped and shared by every other test in this suite),
    killing this one doesn't disturb any other device.

    obs-ptz's own automatic reconnect (PTZViscaOverTCP::
    on_socket_stateChanged() retries connectToHost() every 1.9s, see
    src/ptz-visca-tcp.cpp) picks a freshly (re)started listener back up
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


@pytest.fixture(scope="session")
def obs_world(tmp_path_factory, ptzsim_process, sony_ptzsim, birddog_ptzsim, power_ptzsim, filter_power_ptzsim,
              late_power_ptzsim, no_power_ptzsim):
    home = tmp_path_factory.mktemp("obs-home")
    write_ptz_plugin_config(home, ptzsim_process["ports"], ptzsim_process["serial_paths"])

    ws_port = free_port()
    ws_password = "ptzsim-ci-password"
    write_websocket_config(home, ws_port, ws_password)

    obs_binary = os.environ.get(
        "PTZSIM_OBS_BINARY",
        "/Applications/OBS.app/Contents/MacOS/OBS" if platform.system() == "Darwin" else "obs")

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

    # With no display, start an Xvfb here rather than through xvfb-run: a
    # SIGTERM to xvfb-run ends xvfb-run and leaves OBS and its Xvfb running.
    # OBS then never closes (so never turns cameras off), and the next run's
    # OBS can't start next to it. Launched directly, proc is OBS itself.
    xvfb = None
    if platform.system() == "Linux" and not env.get("DISPLAY") and shutil.which("Xvfb"):
        xvfb, env["DISPLAY"] = start_xvfb()

    # Its own process group, so that whatever OBS starts goes when it does.
    with output_log("obs") as out:
        proc = subprocess.Popen([obs_binary, "--disable-updater"], cwd=home, env=env, stdout=out,
                                stderr=subprocess.STDOUT, start_new_session=True)

    ws = None
    try:
        wait_for_port("127.0.0.1", ws_port, timeout=90)
        deadline = time.time() + 30
        last_error = None
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
                break
            except ObsWebSocketError as e:
                if e.status.get("code") != OBS_NOT_READY:
                    raise
                last_error = e
                time.sleep(0.2)
        else:
            raise RuntimeError(f"OBS frontend never became ready: {last_error}")

        # obs-websocket's SetCurrentProgramScene doesn't wait for the
        # switch to finish -- with the default (animated) transition, a
        # scene switch issued while the previous one is still transitioning
        # can silently fail to activate the new scene's sources at all, so
        # a ptz_action_source never fires. Force an instant Cut so each
        # trigger_action's scene switch always completes immediately.
        ws.call("SetCurrentSceneTransition", {"transitionName": "Cut"})

        # OBS has finished loading (obs-websocket is not ready before), and
        # that was the time devices were told to power their cameras on: this
        # one was not there to be told
        late_power_ptzsim.start()

        yield World(ws, f"http://127.0.0.1:{ptzsim_process['ports']['debug_http']}/state", DEVICE_IDS)
    finally:
        if ws is not None:
            ws.close()
        # SIGINT, not SIGTERM: OBS closes its main window -- a normal quit --
        # on SIGINT since 23.2, but only on SIGTERM since 32.1. Before that
        # a SIGTERM just kills it, and nothing gets turned off.
        exited_cleanly = stop_process_group(proc, timeout=10, sig=signal.SIGINT)
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


@pytest.fixture(autouse=True)
def _cleanup_scenes_between_tests(obs_world):
    yield
    obs_world.cleanup_scenes()
