"""A user's VISCA command sets, read from the plugin config's
"visca-profiles" directory (conftest.py's VISCA_PROFILES writes them).

Each test has a ptzsim of its own that says it is the model one of them is
for (--visca-model), and a source with a VISCA filter pointed at it.
"""

import itertools
import json
import subprocess
import sys
import urllib.request

from obsws import ObsWebSocketError

from conftest import REPO_ROOT, free_port, output_log, wait_for_port

FILTER_KIND = "ca.secretlab.obs-ptz.visca"

_sources = itertools.count(1)


class Sim:
    """A VISCA-over-TCP ptzsim that says it is `model`"""

    def __init__(self, model):
        self.tcp_port = free_port()
        self.debug_port = free_port()
        cmd = [
            sys.executable, "-m", "ptzsim",
            "--host", "127.0.0.1",
            "--visca-tcp-port", str(self.tcp_port),
            "--visca-model", model,
            "--no-visca-udp", "--no-visca-serial", "--no-onvif", "--no-pelco",
            "--debug-http-port", str(self.debug_port),
        ]
        with output_log("ptzsim-user-profile") as out:
            self.proc = subprocess.Popen(cmd, cwd=REPO_ROOT / "scripts", stdout=out, stderr=subprocess.STDOUT)
        wait_for_port("127.0.0.1", self.debug_port, timeout=15)
        wait_for_port("127.0.0.1", self.tcp_port, timeout=15)

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()

    def state(self):
        with urllib.request.urlopen(f"http://127.0.0.1:{self.debug_port}/state", timeout=5) as resp:
            return json.load(resp)


class Camera:
    def __init__(self, request, world, tmp_path, model):
        self.world = world
        self.tmp_path = tmp_path
        self.sim = Sim(model)
        request.addfinalizer(self.sim.stop)
        self.source = f"user-profile-cam-{next(_sources)}"
        world.ws.call("CreateInput", {
            "sceneName": world.create_scene(),
            "inputName": self.source,
            "inputKind": "ffmpeg_source",
            "inputSettings": {"is_local_file": True, "local_file": ""},
        })
        request.addfinalizer(self.remove)
        world.ws.call("CreateSourceFilter", {
            "sourceName": self.source,
            "filterName": "PTZ",
            "filterKind": FILTER_KIND,
            "filterSettings": {"type": "visca-over-tcp", "host": "127.0.0.1", "tcp_port": self.sim.tcp_port},
        })
        self.device_id = world.wait_for_device_by_name(
            self.source, tmp_path / "device.json", lambda r: r["found"] and r["bound"])["device_id"]

    def remove(self):
        try:
            self.world.ws.call("RemoveInput", {"inputName": self.source})
        except ObsWebSocketError:
            pass

    def wait_for_state(self, predicate, timeout=15):
        return self.world.wait_for_device_state(
            self.device_id, self.tmp_path / "state.json", lambda r: predicate(r["state"]), timeout=timeout)["state"]


def test_a_user_state_value_is_read_and_set(request, obs_world, tmp_path):
    camera = Camera(request, obs_world, tmp_path, "0123:0001")
    state = camera.wait_for_state(lambda s: {"user_wb", "user_zoom", "wb_mode", "zoom_pos"} <= set(s))
    assert state["user_wb"] == camera.sim.state()["visca"]["wb_mode"]
    assert state["user_zoom"] == state["zoom_pos"]
    assert "low_latency" not in state

    obs_world.run_ui_test("set_device_state", device_id=camera.device_id, user_wb=5)
    obs_world.wait_for(lambda: camera.sim.state()["visca"]["wb_mode"] == 5, timeout=5)
    camera.wait_for_state(lambda s: s.get("user_wb") == 5, timeout=5)


def test_a_user_trigger_is_sent(request, obs_world, tmp_path):
    camera = Camera(request, obs_world, tmp_path, "0123:0001")
    camera.wait_for_state(lambda s: "pan" in s)
    obs_world.run_ui_test("move_device", device_id=camera.device_id, mode="abs", pan=0.5, tilt=0.0)
    obs_world.wait_for(lambda: camera.sim.state()["pan"] > 0.1, timeout=5)

    obs_world.run_ui_test("trigger_device", device_id=camera.device_id, name="user_home")
    obs_world.wait_for(lambda: abs(camera.sim.state()["pan"]) < 0.005, timeout=5)


def test_a_command_set_extends_another(request, obs_world, tmp_path):
    camera = Camera(request, obs_world, tmp_path, "0123:0002")
    state = camera.wait_for_state(lambda s: "wb_mode" in s and "power_on" in s)
    # what it extends took this away, and it took away the other
    assert "low_latency" not in state
    assert "user_wb" not in state


def test_a_command_set_that_cant_be_read_is_left_out(request, obs_world, tmp_path):
    camera = Camera(request, obs_world, tmp_path, "0123:0003")
    # the generic command set, which has it
    camera.wait_for_state(lambda s: "low_latency" in s)


def test_the_dock_offers_only_what_a_command_set_has(request, obs_world, tmp_path):
    """The command set has no zoom drive, so the camera can't zoom at a
    speed: the dock's zoom buttons are off, but it still pans"""
    camera = Camera(request, obs_world, tmp_path, "0123:0001")
    state = camera.wait_for_state(lambda s: s.get("features", {}).get("pantilt") is True)
    assert "zoom" not in state["features"]
    assert state["features"]["zoom_abs"] is True
    enabled = obs_world.wait_for_dock_controls(
        camera.device_id, tmp_path / "dock.json", lambda e: e["panTiltButton_up"])
    assert enabled["zoomButton_tele"] is False
    assert enabled["zoomButton_wide"] is False


def test_every_shipped_command_set_is_offered(obs_world, tmp_path):
    """Each file in src/visca-profiles is linked into the plugin and read, so
    one with anything wrong with it, which would be left out, fails here"""
    shipped = {json.loads(path.read_text())["id"] for path in (REPO_ROOT / "src" / "visca-profiles").glob("*.json")}
    offered = obs_world.device_settings(obs_world.device_ids["visca-tcp"], tmp_path / "settings.json")["lists"]
    offered = offered["visca_profile"]
    assert shipped <= set(offered)
    assert {"auto", "generic"} <= set(offered)
    assert len(offered) == len(set(offered))


def test_a_users_command_set_replaces_a_shipped_one(request, obs_world, tmp_path):
    """conftest.py's birddog-p100.json has the shipped P100's id, and is used
    instead of it: for its other model too, and without low_latency"""
    camera = Camera(request, obs_world, tmp_path, "0123:0004")
    state = camera.wait_for_state(lambda s: "wb_mode" in s and "power_on" in s)
    assert "low_latency" not in state


def test_a_command_set_doesnt_change_the_one_it_extends(request, obs_world, tmp_path):
    """control-only.json says the AE mode can't be set, and changes nothing
    else; the generic command set, which it extends, still can, for a camera
    of a model no command set is for"""
    camera = Camera(request, obs_world, tmp_path, "0001:0000")
    camera.wait_for_state(lambda s: "ae_mode" in s)
    obs_world.run_ui_test("set_device_state", device_id=camera.device_id, ae_mode=3)
    obs_world.wait_for(lambda: camera.sim.state()["visca"]["ae_mode"] == 3, timeout=5)
