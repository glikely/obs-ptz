"""Covers PTZ devices that belong to an OBS filter: adding a "VISCA PTZ
Control" filter to a source creates a PTZDevice for that source, which
lives as long as the filter does, instead of the device being set up in
the plugin's own device list (see ptz_visca_filter_info in
src/ptz-device.cpp).

Each test creates a media source to stand in for the camera (an empty
one is enough), adds the filter to it over obs-websocket (CreateSourceFilter) with the
connection settings for a dedicated ptzsim instance, and finds the
device through tests/ui-harness/device-source-test.cpp's
"get_device_source" test -- by name, since a filter's device is given
its id by the plugin, not by the test's config file. What the tests check:

- the filter kind exists, and adding one creates a device named after,
  and bound to, the source it was added to;
- the device is usable: it connects to the camera, and an action
  (ptz_action_source, as everywhere in this suite) reaches it;
- it follows the source's name, and shows as live when the source is in
  the program scene, including when the filter is added after the scene
  went live, which no scene change announces;
- removing the filter removes the device, leaving the source and any
  other filter's device alone;
- a filter's device is saved with the filter, not in the plugin's own
  list of devices.
"""

import json
import subprocess
import sys
import time
import types
import urllib.request

import pytest
from obsws import ObsWebSocketError

from conftest import REPO_ROOT, free_port, output_log, wait_for_port

FILTER_KIND = "ca.secretlab.obs-ptz.visca"

ACTION_PAN_TILT = 3
ACTION_STOP = 4


class FilterPtzsim:
    """A dedicated, VISCA-over-TCP-only ptzsim instance, with its own
    debug port to read the camera's state from, that filter devices can
    be pointed at without disturbing the suite's shared one."""

    def __init__(self):
        self.tcp_port = free_port()
        self.debug_port = free_port()
        self.debug_url = f"http://127.0.0.1:{self.debug_port}/state"
        self.proc = None

    def start(self):
        cmd = [
            sys.executable, "-m", "ptzsim",
            "--host", "127.0.0.1",
            "--visca-tcp-port", str(self.tcp_port),
            "--no-visca-udp", "--no-visca-serial", "--no-onvif", "--no-pelco",
            "--debug-http-port", str(self.debug_port),
        ]
        with output_log("ptzsim-filter") as out:
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


@pytest.fixture
def camera_sim():
    sim = FilterPtzsim()
    sim.start()
    yield sim
    sim.stop()


class Cameras:
    """Creates the sources that stand in for cameras, and adds and removes
    filters on them. Removes the sources again at the end, since removing
    a scene doesn't remove the inputs in it."""

    def __init__(self, world, sim):
        self.world = world
        self.sim = sim
        self.names = set()

    def add_source(self, scene, name):
        """Adds an empty media source. It has to be one like a camera, and
        not something like a colour source: the filter has no video of its
        own, and OBS takes a filter like that for an audio/video (async)
        filter, which it only lets be added to an async source, and
        silently refuses to add to anything else."""
        self.world.ws.call("CreateInput", {
            "sceneName": scene,
            "inputName": name,
            "inputKind": "ffmpeg_source",
            "inputSettings": {"is_local_file": True, "local_file": ""},
        })
        self.names.add(name)

    def add_filter(self, source, filter_name="PTZ"):
        self.world.ws.call("CreateSourceFilter", {
            "sourceName": source,
            "filterName": filter_name,
            "filterKind": FILTER_KIND,
            "filterSettings": {
                "type": "visca-over-tcp",
                "host": "127.0.0.1",
                "tcp_port": self.sim.tcp_port,
            },
        })

    def remove_filter(self, source, filter_name="PTZ"):
        self.world.ws.call("RemoveSourceFilter", {"sourceName": source, "filterName": filter_name})

    def rename(self, old, new):
        self.world.ws.call("SetInputName", {"inputName": old, "newInputName": new})
        self.names.discard(old)
        self.names.add(new)

    def cleanup(self):
        for name in list(self.names):
            try:
                self.world.ws.call("RemoveInput", {"inputName": name})
            except ObsWebSocketError:
                pass


@pytest.fixture
def cameras(obs_world, camera_sim):
    c = Cameras(obs_world, camera_sim)
    yield c
    c.cleanup()


@pytest.fixture
def unreachable_cameras(obs_world):
    """Cameras whose filters are pointed at a port nothing listens on."""
    c = Cameras(obs_world, types.SimpleNamespace(tcp_port=free_port()))
    yield c
    c.cleanup()


def make_program(world, scene):
    world.ws.call("SetCurrentProgramScene", {"sceneName": scene})


def test_filter_kind_is_registered(obs_world):
    # (GetSourceFilterKindList would be the obvious request, but
    # obs-websocket only has it since 5.4)
    settings = obs_world.ws.call("GetSourceFilterDefaultSettings", {"filterKind": FILTER_KIND})
    assert settings["defaultFilterSettings"]["type"] == "visca-over-ip"


def test_adding_a_filter_creates_a_device_for_its_source(obs_world, cameras, tmp_path):
    out = tmp_path / "device.json"
    assert obs_world.device_by_name("filter-cam-create", out)["found"] is False

    cameras.add_source(obs_world.create_scene(), "filter-cam-create")
    cameras.add_filter("filter-cam-create")

    device = obs_world.wait_for_device_by_name("filter-cam-create", out, lambda r: r["found"] and r["bound"])
    assert device["source"] == "filter-cam-create"
    assert device["device_id"] > 0
    # Not one of the devices in the plugin's config
    assert device["device_id"] not in obs_world.device_ids.values()


def test_filter_device_says_its_source_with_its_proc(obs_world, cameras, tmp_path):
    """A filter's device says the source the filter is on when asked with
    ptz_get_parent_source, as the device list does to show its video, and
    keeps saying it when the source is renamed."""
    out = tmp_path / "device.json"
    cameras.add_source(obs_world.create_scene(), "filter-cam-proc")
    cameras.add_filter("filter-cam-proc")

    device = obs_world.wait_for_device_by_name("filter-cam-proc", out, lambda r: r["found"] and r["proc_bound"])
    assert device["proc_source"] == "filter-cam-proc"
    assert device["proc_source_uuid"]
    assert device["proc_source_uuid"] == device["source_uuid"]

    cameras.rename("filter-cam-proc", "filter-cam-proc-renamed")

    renamed = obs_world.wait_for_device_source(device["device_id"], out,
                                               lambda r: r["proc_source"] == "filter-cam-proc-renamed")
    assert renamed["proc_source_uuid"] == device["proc_source_uuid"]


def test_filter_device_is_named_after_its_source_without_a_camera(obs_world, unreachable_cameras, tmp_path):
    """The device is in the device list under its source's name as soon as the
    filter is added, not only once the camera answers -- which is when the
    device happens to announce a change of state that carries the name with
    it, so the other tests can't tell."""
    out = tmp_path / "device.json"
    unreachable_cameras.add_source(obs_world.create_scene(), "filter-cam-dead")
    unreachable_cameras.add_filter("filter-cam-dead")

    device = obs_world.wait_for_device_by_name("filter-cam-dead", out, lambda r: r["found"] and r["bound"])
    assert device["source"] == "filter-cam-dead"
    status = obs_world.device_status(device["device_id"], tmp_path / "status.json")
    assert status["connected"] is False


def test_filter_device_controls_the_camera(obs_world, cameras, camera_sim, tmp_path):
    out = tmp_path / "device.json"
    cameras.add_source(obs_world.create_scene(), "filter-cam-use")
    cameras.add_filter("filter-cam-use")
    device_id = obs_world.wait_for_device_by_name("filter-cam-use", out, lambda r: r["found"])["device_id"]

    obs_world.wait_for_device_status(device_id, tmp_path / "status.json", lambda s: s["connected"] is True, timeout=10)

    def sim_state(predicate):
        last = None
        end = time.time() + 5
        while time.time() < end:
            with urllib.request.urlopen(camera_sim.debug_url, timeout=5) as resp:
                last = json.loads(resp.read())
            if predicate(last):
                return last
            time.sleep(0.1)
        raise AssertionError(f"camera state never matched; last seen: {last}")

    obs_world.trigger_action(device_id, ACTION_PAN_TILT, pan_speed=0.4, tilt_speed=0.0)
    sim_state(lambda s: abs(s["pan_speed"] - 0.4) < 0.05)
    obs_world.trigger_action(device_id, ACTION_STOP)
    sim_state(lambda s: s["pan_speed"] == 0.0 and s["tilt_speed"] == 0.0)


def test_filter_device_follows_its_source(obs_world, cameras, tmp_path):
    out = tmp_path / "device.json"
    cameras.add_source(obs_world.create_scene(), "filter-cam-rename")
    cameras.add_filter("filter-cam-rename")
    device_id = obs_world.wait_for_device_by_name("filter-cam-rename", out, lambda r: r["found"] and r["bound"])["device_id"]

    cameras.rename("filter-cam-rename", "filter-cam-renamed")

    result = obs_world.wait_for_device_source(
        device_id, out, lambda r: r["name"] == "filter-cam-renamed" and r["config_name"] == "filter-cam-renamed")
    assert result["source"] == "filter-cam-renamed"
    assert result["device_id"] == device_id


def test_filter_device_is_live_when_its_source_is(obs_world, cameras, tmp_path):
    out = tmp_path / "device.json"
    scene = obs_world.create_scene()
    cameras.add_source(scene, "filter-cam-live")
    make_program(obs_world, scene)

    # The scene is already live when the filter is added, and nothing
    # announces that: the device has to look for itself
    cameras.add_filter("filter-cam-live")
    result = obs_world.wait_for_device_by_name("filter-cam-live", out, lambda r: r["found"] and r["live"], timeout=10)
    assert result["locked"] is True
    device_id = result["device_id"]

    make_program(obs_world, obs_world.create_scene())
    result = obs_world.wait_for_device_source(device_id, out, lambda r: not r["live"], timeout=10)
    assert result["bound"] is True
    assert result["locked"] is False


def test_removing_a_filter_removes_its_device(obs_world, cameras, tmp_path):
    out = tmp_path / "device.json"
    cameras.add_source(obs_world.create_scene(), "filter-cam-remove")
    cameras.add_filter("filter-cam-remove")
    obs_world.wait_for_device_by_name("filter-cam-remove", out, lambda r: r["found"])

    cameras.remove_filter("filter-cam-remove")

    obs_world.wait_for_device_by_name("filter-cam-remove", out, lambda r: not r["found"], timeout=10)
    # The source itself is untouched
    assert any(i["inputName"] == "filter-cam-remove" for i in obs_world.ws.call("GetInputList")["inputs"])


def test_each_filter_has_its_own_device(obs_world, cameras, tmp_path):
    out = tmp_path / "device.json"
    scene = obs_world.create_scene()
    cameras.add_source(scene, "filter-cam-a")
    cameras.add_source(scene, "filter-cam-b")
    cameras.add_filter("filter-cam-a")
    cameras.add_filter("filter-cam-b")

    a = obs_world.wait_for_device_by_name("filter-cam-a", out, lambda r: r["found"] and r["bound"])
    b = obs_world.wait_for_device_by_name("filter-cam-b", out, lambda r: r["found"] and r["bound"])
    assert a["device_id"] != b["device_id"]
    assert a["source_uuid"] != b["source_uuid"]

    cameras.remove_filter("filter-cam-a")
    obs_world.wait_for_device_by_name("filter-cam-a", out, lambda r: not r["found"], timeout=10)
    assert obs_world.device_by_name("filter-cam-b", out)["device_id"] == b["device_id"]


def test_filter_devices_are_not_saved_in_the_device_list(obs_world, cameras, tmp_path):
    out = tmp_path / "device.json"
    cameras.add_source(obs_world.create_scene(), "filter-cam-save")
    cameras.add_filter("filter-cam-save")
    device_id = obs_world.wait_for_device_by_name("filter-cam-save", out, lambda r: r["found"])["device_id"]

    saved = obs_world.saved_devices(tmp_path / "saved.json")

    saved_ids = {d["id"] for d in saved}
    assert device_id not in saved_ids
    # ...while the devices in the plugin's own config are
    assert set(obs_world.device_ids.values()) <= saved_ids
