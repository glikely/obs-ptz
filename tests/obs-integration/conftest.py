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
import shutil
import socket
import subprocess
import sys
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
            "port": ports["visca_tcp"],
        },
        {
            "id": DEVICE_IDS["visca-udp"],
            "name": "sim-visca-udp",
            "type": "visca-over-ip",
            "host": "127.0.0.1",
            "port": ports["visca_udp"],
        },
        {
            "id": DEVICE_IDS["visca-serial"],
            "name": "sim-visca-serial",
            "type": "visca",
            "port": str(serial_paths["visca_serial"]),
            "address": 1,
        },
        {
            "id": DEVICE_IDS["pelco-d"],
            "name": "sim-pelco-d",
            "type": "pelco",
            "port": str(serial_paths["pelco"]),
            "address": 1,
            "use_pelco_d": True,
        },
        {
            "id": DEVICE_IDS["pelco-p"],
            "name": "sim-pelco-p",
            "type": "pelco",
            "port": str(serial_paths["pelco"]),
            "address": 1,
            "use_pelco_d": False,
        },
    ]
    (config_dir / "config.json").write_text(json.dumps({"devices": devices}))


def wait_for_port(host, port, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=1):
                return
        except OSError:
            time.sleep(0.2)
    raise TimeoutError(f"nothing listening on {host}:{port} after {timeout}s")


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
        "visca_udp": free_port(),
        "onvif_http": free_port(),
        "debug_http": free_port(),
        "rtsp": free_port(),
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
    proc = subprocess.Popen(cmd, cwd=REPO_ROOT / "scripts", stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True)
    wait_for_port("127.0.0.1", ptz_ports["debug_http"], timeout=15)
    yield {"proc": proc, "ports": ptz_ports, "serial_paths": serial_paths}
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()


@pytest.fixture(scope="session")
def obs_world(tmp_path_factory, ptzsim_process):
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

    cmd = [obs_binary, "--disable-updater"]
    if platform.system() == "Linux" and not env.get("DISPLAY") and shutil.which("xvfb-run"):
        cmd = ["xvfb-run", "-a"] + cmd

    proc = subprocess.Popen(cmd, cwd=home, env=env, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True)

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

        yield World(ws, f"http://127.0.0.1:{ptzsim_process['ports']['debug_http']}/state", DEVICE_IDS)
    finally:
        if ws is not None:
            ws.close()
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()


@pytest.fixture(autouse=True)
def _cleanup_scenes_between_tests(obs_world):
    yield
    obs_world.cleanup_scenes()
