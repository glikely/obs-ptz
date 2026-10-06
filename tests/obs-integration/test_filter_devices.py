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
- it shows as in the preview in Studio Mode, and is locked while live;
  a lock a user sets is dropped by the next scene change, and one a user
  lifts from a live device stays lifted until then;
- removing the filter removes the device, leaving the source and any
  other filter's device alone;
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
    assert device["uuid"]


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

    renamed = obs_world.wait_for_device_source(device["uuid"], out,
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
    status = obs_world.device_status(device["uuid"], tmp_path / "status.json")
    assert status["connected"] is False


def test_filter_device_controls_the_camera(obs_world, cameras, camera_sim, tmp_path):
    out = tmp_path / "device.json"
    cameras.add_source(obs_world.create_scene(), "filter-cam-use")
    cameras.add_filter("filter-cam-use")
    device_name = obs_world.wait_for_device_by_name("filter-cam-use", out, lambda r: r["found"])["uuid"]

    obs_world.wait_for_device_status(device_name, tmp_path / "status.json", lambda s: s["connected"] is True, timeout=10)

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

    obs_world.trigger_action(device_name, ACTION_PAN_TILT, pan_speed=0.4, tilt_speed=0.0)
    sim_state(lambda s: abs(s["pan_speed"] - 0.4) < 0.05)
    obs_world.trigger_action(device_name, ACTION_STOP)
    sim_state(lambda s: s["pan_speed"] == 0.0 and s["tilt_speed"] == 0.0)


def test_filter_device_follows_its_source(obs_world, cameras, tmp_path):
    out = tmp_path / "device.json"
    cameras.add_source(obs_world.create_scene(), "filter-cam-rename")
    cameras.add_filter("filter-cam-rename")
    device_name = obs_world.wait_for_device_by_name("filter-cam-rename", out, lambda r: r["found"] and r["bound"])["uuid"]

    cameras.rename("filter-cam-rename", "filter-cam-renamed")

    result = obs_world.wait_for_device_source(
        device_name, out, lambda r: r["name"] == "filter-cam-renamed")
    assert result["source"] == "filter-cam-renamed"
    assert result["uuid"] == device_name


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
    device_name = result["uuid"]

    make_program(obs_world, obs_world.create_scene())
    result = obs_world.wait_for_device_source(device_name, out, lambda r: not r["live"], timeout=10)
    assert result["bound"] is True
    assert result["locked"] is False


def set_studio_mode(world, enabled):
    world.ws.call("SetStudioModeEnabled", {"studioModeEnabled": enabled})
    world.wait_for(lambda: world.ws.call("GetStudioModeEnabled")["studioModeEnabled"] == enabled, timeout=5)


@pytest.fixture
def studio_mode(obs_world):
    """Studio Mode, which OBS starts in whichever way it was last left"""
    set_studio_mode(obs_world, True)
    yield
    set_studio_mode(obs_world, False)


def test_filter_device_is_in_preview_when_its_source_is(obs_world, cameras, studio_mode, tmp_path):
    out = tmp_path / "device.json"
    cam_scene = obs_world.create_scene()
    empty_scene = obs_world.create_scene()
    cameras.add_source(cam_scene, "filter-cam-preview")
    cameras.add_filter("filter-cam-preview")
    device = obs_world.wait_for_device_by_name("filter-cam-preview", out, lambda r: r["found"] and r["bound"])["uuid"]
    make_program(obs_world, empty_scene)

    obs_world.ws.call("SetCurrentPreviewScene", {"sceneName": cam_scene})
    result = obs_world.wait_for_device_source(device, out, lambda r: r["preview"], timeout=10)
    assert result["live"] is False
    assert result["locked"] is False

    obs_world.ws.call("SetCurrentPreviewScene", {"sceneName": empty_scene})
    obs_world.wait_for_device_source(device, out, lambda r: not r["preview"], timeout=10)


def test_filter_device_is_not_in_preview_outside_studio_mode(obs_world, cameras, tmp_path):
    out = tmp_path / "device.json"
    set_studio_mode(obs_world, False)
    scene = obs_world.create_scene()
    cameras.add_source(scene, "filter-cam-nopreview")
    cameras.add_filter("filter-cam-nopreview")
    device = obs_world.wait_for_device_by_name("filter-cam-nopreview", out, lambda r: r["found"] and r["bound"])["uuid"]
    make_program(obs_world, scene)
    result = obs_world.wait_for_device_source(device, out, lambda r: r["live"], timeout=10)
    assert result["preview"] is False


def test_user_lock_holds_until_the_scene_changes(obs_world, cameras, tmp_path):
    out = tmp_path / "device.json"
    scene = obs_world.create_scene()
    cameras.add_source(scene, "filter-cam-lock")
    cameras.add_filter("filter-cam-lock")
    device = obs_world.wait_for_device_by_name("filter-cam-lock", out, lambda r: r["found"] and r["bound"])["uuid"]
    make_program(obs_world, obs_world.create_scene())
    obs_world.wait_for_device_source(device, out, lambda r: not r["live"], timeout=10)

    obs_world.set_device_locked(device, True)
    result = obs_world.wait_for_device_source(device, out, lambda r: r["locked"], timeout=10)
    assert result["live"] is False

    obs_world.set_device_locked(device, False)
    obs_world.wait_for_device_source(device, out, lambda r: not r["locked"], timeout=10)

    # Locked, then a scene change: the lock is dropped, as a lock on a camera
    # that is no longer the one in use has no point
    obs_world.set_device_locked(device, True)
    obs_world.wait_for_device_source(device, out, lambda r: r["locked"], timeout=10)
    make_program(obs_world, scene)
    result = obs_world.wait_for_device_source(device, out, lambda r: r["live"], timeout=10)
    # ...and the device is locked because it is live, which is no lock of the user's
    assert result["locked"] is True

    make_program(obs_world, obs_world.create_scene())
    result = obs_world.wait_for_device_source(device, out, lambda r: not r["live"], timeout=10)
    assert result["locked"] is False


def test_user_can_unlock_a_live_device_until_the_scene_changes(obs_world, cameras, tmp_path):
    out = tmp_path / "device.json"
    scene = obs_world.create_scene()
    other = obs_world.create_scene()
    cameras.add_source(scene, "filter-cam-unlock")
    make_program(obs_world, scene)
    cameras.add_filter("filter-cam-unlock")
    device = obs_world.wait_for_device_by_name("filter-cam-unlock", out, lambda r: r["found"] and r["live"], timeout=10)["uuid"]

    obs_world.set_device_locked(device, False)
    result = obs_world.wait_for_device_source(device, out, lambda r: not r["locked"], timeout=10)
    assert result["live"] is True

    # A scene change that leaves the device live locks it again
    make_program(obs_world, other)
    obs_world.wait_for_device_source(device, out, lambda r: not r["live"], timeout=10)
    make_program(obs_world, scene)
    result = obs_world.wait_for_device_source(device, out, lambda r: r["live"], timeout=10)
    assert result["locked"] is True


def test_user_lock_is_dropped_when_the_preview_changes(obs_world, cameras, studio_mode, tmp_path):
    out = tmp_path / "device.json"
    scene = obs_world.create_scene()
    empty_scene = obs_world.create_scene()
    cameras.add_source(scene, "filter-cam-prelock")
    cameras.add_filter("filter-cam-prelock")
    device = obs_world.wait_for_device_by_name("filter-cam-prelock", out, lambda r: r["found"] and r["bound"])["uuid"]
    make_program(obs_world, empty_scene)
    obs_world.ws.call("SetCurrentPreviewScene", {"sceneName": empty_scene})

    obs_world.set_device_locked(device, True)
    obs_world.wait_for_device_source(device, out, lambda r: r["locked"], timeout=10)

    obs_world.ws.call("SetCurrentPreviewScene", {"sceneName": scene})
    result = obs_world.wait_for_device_source(device, out, lambda r: r["preview"], timeout=10)
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
    assert a["uuid"] != b["uuid"]
    assert a["source_uuid"] != b["source_uuid"]

    cameras.remove_filter("filter-cam-a")
    obs_world.wait_for_device_by_name("filter-cam-a", out, lambda r: not r["found"], timeout=10)
    assert obs_world.device_by_name("filter-cam-b", out)["uuid"] == b["uuid"]
