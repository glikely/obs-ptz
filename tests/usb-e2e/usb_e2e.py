#!/usr/bin/env python3
"""End-to-end test of a real USB UVC PTZ camera through real OBS and the plugin.

Run by run-macos.sh, which has already isolated OBS's config; see README.md.

usage: usb_e2e.py <repo root> <work dir> <AVFoundation unique ID> <usb-uvc-position>

The plugin keeps the camera's USB device open, and only one process can, so the
camera's position can't be read while OBS is running. Every scenario
therefore launches OBS, drives the plugin over obs-websocket, quits OBS, and
then asks the camera directly with usb-uvc-position. Note that this is the
position the camera *reports*: some cameras report the last position they were
told to go to, not where the motor is, so on those it shows the command arrived
and not that the camera physically moved (see README.md).

Expected positions are worked out from the camera's own ranges, the same way
PTZUsbBackend does, so this isn't tied to one model of camera.
"""
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(sys.argv[1])
WORK = Path(sys.argv[2])
CAMERA_ID = sys.argv[3]
POSITION_TOOL = sys.argv[4]

sys.path.insert(0, str(REPO / "tests" / "obs-integration"))
from obsws import Client, ObsWebSocketError  # noqa: E402

DEVICE_ID = 20
SOURCE = "USB Camera Under Test"
WRAPPER = REPO / "scripts" / "macos-integration-test-obs-wrapper.py"
REAL_OBS = os.environ.get("PTZSIM_REAL_OBS_BINARY", "/Applications/OBS.app/Contents/MacOS/OBS")

# ptz_action_source's actions, see src/ptz-action-source.c
ACTION_PRESET_RECALL = 2
ACTION_PAN_TILT = 3
ACTION_STOP = 4
ACTION_PRESET_SAVE = 5

failures = []


def check(ok, what, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}: {what}{(' -- ' + detail) if detail else ''}", flush=True)
    if not ok:
        failures.append(what)


# ---- the camera, read directly (OBS must not be running)

def read_camera(extra=()):
    result = subprocess.run([POSITION_TOOL, CAMERA_ID, *extra], capture_output=True, text=True)
    ranges, pos = {}, None
    for line in result.stdout.splitlines():
        if line.startswith("RANGES"):
            for axis, lo, hi, step in re.findall(r"(\w+)=(-?\d+):(-?\d+):(-?\d+)", line):
                ranges[axis] = (int(lo), int(hi), int(step))
        elif line.startswith("POS"):
            pos = {k: int(v) for k, v in re.findall(r"(\w+)=(-?\d+)", line)}
    return ranges, pos


def target(norm, axis):
    """Where PTZUsbBackend sends the camera for a normalized position: the
    value scaled by the range's maximum, clamped and rounded onto the step."""
    lo, hi, step = RANGES[axis]
    v = min(max(int(norm * hi), lo), hi)  # int() truncates like static_cast<long>
    if step > 1:
        v = min(max(lo + ((v - lo + step // 2) // step) * step, lo), hi)
    return v


def tolerance(axis):
    lo, hi, step = RANGES[axis]
    return max(step, (hi - lo) // 100)


def at(pos, want):
    """Whether the camera is at want ({axis: raw}), give or take a step"""
    return pos is not None and all(abs(pos[a] - v) <= tolerance(a) for a, v in want.items())


# ---- OBS, one launch at a time

iso_home = Path(tempfile.mkdtemp(prefix="home-", dir=WORK))
config_dir = iso_home / "Library" / "Application Support" / "obs-studio" / "plugin_config" / "obs-ptz"
config_dir.mkdir(parents=True)
(config_dir / "config.json").write_text(json.dumps({"devices": [{"id": DEVICE_ID, "name": SOURCE, "type": "usb-cam"}]}))


LAUNCHED = []


class Obs:
    """One launch of OBS, with the plugin's UI-test harness reachable over
    obs-websocket. The wrapper puts the plugin's config where OBS really reads it."""

    def __init__(self, label):
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]
        env = dict(os.environ, HOME=str(iso_home), OBS_WEBSOCKET_SERVER_ENABLE="true",
                   OBS_WEBSOCKET_SERVER_PORT=str(port), OBS_WEBSOCKET_SERVER_PASSWORD="",
                   PTZ_UI_TEST_HARNESS="1", PTZSIM_REAL_OBS_BINARY=REAL_OBS)
        self.out = open(WORK / f"obs-stdout-{label}.txt", "w")
        self.proc = subprocess.Popen([str(WRAPPER), "--disable-updater"], cwd=iso_home, env=env, stdout=self.out,
                                     stderr=subprocess.STDOUT)
        LAUNCHED.append(self)
        self.scenes = []
        self.ws = None
        deadline = time.time() + 120
        while time.time() < deadline and self.ws is None:
            if self.proc.poll() is not None:
                raise SystemExit(f"OBS exited early ({self.proc.returncode})")
            try:
                self.ws = Client(f"ws://127.0.0.1:{port}", timeout=3)
            except Exception:
                time.sleep(1)
        if self.ws is None:
            raise SystemExit("could not connect to obs-websocket")
        while True:  # OBS answers 207 (not ready) for a while after the handshake
            try:
                self.ws.call("GetVersion")
                break
            except ObsWebSocketError as e:
                if e.status.get("code") != 207:
                    raise
                time.sleep(1)

    def ui(self, cmd, **params):
        return self.ws.call("CallVendorRequest", {
            "vendorName": "obs-ptz", "requestType": "ui_test_run",
            "requestData": {"cmd": cmd, **{k: str(v) for k, v in params.items()}}})

    def status(self):
        out = WORK / "status.json"
        if out.exists():
            out.unlink()
        self.ui("get_device_status", device_id=DEVICE_ID, filename=str(out))
        for _ in range(50):
            if out.exists():
                try:
                    return json.loads(out.read_text())
                except ValueError:
                    pass
            time.sleep(0.1)
        return {}

    def wait_connected(self, timeout=25):
        end = time.time() + timeout
        while time.time() < end:
            if self.status().get("connected") is True:
                return True
            time.sleep(0.5)
        return False

    def add_camera_source(self):
        self.ws.call("CreateScene", {"sceneName": "usb-e2e"})
        self.ws.call("CreateInput", {"sceneName": "usb-e2e", "inputName": SOURCE, "inputKind": "macos-avcapture",
                                     "inputSettings": {"device": CAMERA_ID}})
        self.ws.call("SetCurrentProgramScene", {"sceneName": "usb-e2e"})

    def action(self, action, preset=0, pan=0.0, tilt=0.0):
        """What a hotkey or joystick does: a ptz_action_source coming to program"""
        scene = f"action-{time.time_ns()}"
        self.scenes.append(scene)
        self.ws.call("CreateScene", {"sceneName": scene})
        self.ws.call("CreateInput", {"sceneName": scene, "inputName": scene + "-source",
                                     "inputKind": "ptz_action_source",
                                     "inputSettings": {"trigger": 0, "device_id": DEVICE_ID, "action": action,
                                                       "preset_id": preset, "pan_speed": pan, "tilt_speed": tilt}})
        self.ws.call("SetCurrentProgramScene", {"sceneName": scene})

    def move(self, **position):
        self.ui("move_device", device_id=DEVICE_ID, mode="abs", **position)

    def quit(self):
        # Leave nothing behind that would fire an action when OBS next starts
        try:
            self.ws.call("SetCurrentProgramScene", {"sceneName": "usb-e2e"})
            for scene in self.scenes:
                self.ws.call("RemoveScene", {"sceneName": scene})
        except Exception as e:
            print(f"  (cleanup: {e})")
        self.proc.terminate()
        try:
            self.proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        time.sleep(2)  # let go of the USB device


# ---- the scenarios

RANGES, START = read_camera()
if START is None:
    sys.exit(f"could not read the camera {CAMERA_ID}: is it attached, and is nothing else using it?")
for axis in ("pan", "tilt", "zoom"):
    if axis not in RANGES or RANGES[axis][1] <= RANGES[axis][0]:
        sys.exit(f"this camera has no {axis} control; the test needs pan, tilt and zoom")

AWAY = {"pan": target(0.10, "pan"), "tilt": target(0.10, "tilt"), "zoom": target(0.25, "zoom")}
HOME = {"pan": target(0.0, "pan"), "tilt": target(0.0, "tilt"), "zoom": target(0.0, "zoom")}

try:
    print(f"camera {CAMERA_ID}, ranges {RANGES}, starting at {START}")

    print("\n== 1. bind a USB device to the camera's source, move it, quit OBS, read the camera")
    obs = Obs("1")
    obs.add_camera_source()
    check(obs.wait_connected(), "the device reports connected", str(obs.status()))
    _, during = read_camera()
    print("  read by another process while OBS runs:", during if during else "no (the plugin holds the device open)")
    obs.move(pan=0.10, tilt=0.10, zoom=0.25)
    time.sleep(4.5)
    obs.quit()
    _, pos = read_camera()
    check(at(pos, AWAY), f"the camera is at {AWAY}", str(pos))

    print("\n== 2. relaunch with the camera away from home: save a preset first, go home, recall it")
    obs = Obs("2")
    check(obs.wait_connected(), "the device reconnects after a relaunch", str(obs.status()))
    obs.action(ACTION_PRESET_SAVE, preset=1)  # the position the plugin read from the camera when it opened
    time.sleep(1.0)
    obs.move(pan=0.0, tilt=0.0, zoom=0.0)
    time.sleep(4.5)
    obs.action(ACTION_PRESET_RECALL, preset=1)
    time.sleep(4.5)
    obs.quit()
    _, pos = read_camera()
    check(at(pos, AWAY), "recall returned the camera to the position saved at startup", str(pos))

    print("\n== 3. relaunch: a continuous move, the way a hotkey or joystick drives it, then home")
    obs = Obs("3")
    check(obs.wait_connected(), "the device is connected", str(obs.status()))
    obs.action(ACTION_PAN_TILT, pan=0.15)
    time.sleep(1.5)
    obs.action(ACTION_STOP)
    time.sleep(3.0)
    obs.quit()
    _, pos = read_camera()
    hi = RANGES["pan"][1]
    print(f"  after the continuous move: {pos}")
    check(pos is not None and pos["pan"] >= AWAY["pan"] + min(0.10 * hi, (hi - AWAY["pan"]) // 2),
          "it kept panning from where it was", str(pos))
    check(pos is not None and abs(pos["tilt"] - AWAY["tilt"]) <= tolerance("tilt"), "without tilting", str(pos))

    obs = Obs("3b")
    obs.wait_connected()
    obs.move(pan=0.0, tilt=0.0, zoom=0.0)
    time.sleep(4.5)
    obs.quit()
    _, pos = read_camera()
    check(at(pos, HOME), "and home again", str(pos))
finally:
    # Don't leave the camera wherever a failure stopped things. That needs OBS
    # out of the way first, since it holds the camera open.
    for launched in LAUNCHED:
        if launched.proc.poll() is None:
            launched.quit()
    _, pos = read_camera()
    if pos is not None and not at(pos, HOME):
        print(f"\nsending the camera home (it was at {pos})")
        read_camera(("--home",))
        time.sleep(4.5)

print(f"\n{'ALL CHECKS PASSED' if not failures else 'FAILED: ' + '; '.join(failures)}")
sys.exit(1 if failures else 0)
