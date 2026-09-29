"""VISCA tally lamps: PTZVisca::onSceneChanged() and requestState() in
src/ptz-visca.cpp.

A VISCA camera with tally lamps has the red one lit while its source is in
the program scene, and the green one lit while it is in the preview scene
(Studio Mode) and not in the program scene. Which lamps a camera has varies:
BirdDog's X4 series has both, a P100 only the red one, and Sony's own
cameras (measured on an SRG-120DH) neither. A camera that lacks a lamp
answers its command with a syntax error, which is then not sent again.

Each test has its own ptzsim (see TallySim) so it starts with the lamps out,
and a source with a VISCA filter on it, so that the device follows the
source's scenes. ptzsim reports the lamps in its debug state.
"""

import json
import subprocess
import sys
import time
import urllib.request

import pytest
from obsws import ObsWebSocketError

from conftest import REPO_ROOT, free_port, output_log, wait_for_port

FILTER_KIND = "ca.secretlab.obs-ptz.visca"
SOURCE = "tally-cam"


class TallySim:
    """A VISCA-over-TCP ptzsim on its own ports, that can be made to lack
    the green lamp."""

    def __init__(self, green, completions=True):
        self.green = green
        self.completions = completions
        self.tcp_port = free_port()
        self.debug_port = free_port()
        self.proc = None

    def start(self):
        cmd = [
            sys.executable, "-m", "ptzsim",
            "--host", "127.0.0.1",
            "--visca-tcp-port", str(self.tcp_port),
            "--no-visca-udp", "--no-visca-serial", "--no-onvif", "--no-pelco",
            "--debug-http-port", str(self.debug_port),
        ]
        if not self.green:
            cmd.append("--visca-no-green-tally")
        if not self.completions:
            cmd.append("--visca-no-completions")
        with output_log("ptzsim-tally") as out:
            self.proc = subprocess.Popen(cmd, cwd=REPO_ROOT / "scripts", stdout=out, stderr=subprocess.STDOUT)
        wait_for_port("127.0.0.1", self.debug_port, timeout=15)
        wait_for_port("127.0.0.1", self.tcp_port, timeout=15)

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()

    def lamps(self):
        with urllib.request.urlopen(f"http://127.0.0.1:{self.debug_port}/state", timeout=5) as resp:
            return json.load(resp)["tally"]


class Tally:
    """A source with a VISCA filter pointed at `sim`, in scene `scene`, and
    another, empty scene `other` to move to."""

    def __init__(self, world, sim, tmp_path, tally_auto=True):
        self.world = world
        self.sim = sim
        # OBS starts in whichever mode it was last left in
        self.studio(False)
        self.scene = world.create_scene()
        self.other = world.create_scene()
        world.ws.call("CreateInput", {
            "sceneName": self.scene,
            "inputName": SOURCE,
            "inputKind": "ffmpeg_source",
            "inputSettings": {"is_local_file": True, "local_file": ""},
        })
        world.ws.call("CreateSourceFilter", {
            "sourceName": SOURCE,
            "filterName": "PTZ",
            "filterKind": FILTER_KIND,
            "filterSettings": {
                "type": "visca-over-tcp",
                "host": "127.0.0.1",
                "tcp_port": sim.tcp_port,
                "tally_auto": tally_auto,
            },
        })
        out = tmp_path / "device.json"
        self.device_id = world.wait_for_device_by_name(SOURCE, out, lambda r: r["found"] and r["bound"])["device_id"]
        world.wait_for_device_status(self.device_id, tmp_path / "status.json", lambda s: s["connected"], timeout=10)
        self.tmp_path = tmp_path
        self.program(self.other)

    def program(self, scene):
        """Makes `scene` the program scene, and waits for the transition to
        it to finish: a switch made before it has is what ends it early, and
        then the scene never becomes the program scene."""
        self.world.ws.call("SetCurrentProgramScene", {"sceneName": scene})
        self.world.wait_for(
            lambda: self.world.ws.call("GetCurrentProgramScene")["currentProgramSceneName"] == scene, timeout=5)

    def studio(self, enabled):
        self.world.ws.call("SetStudioModeEnabled", {"studioModeEnabled": enabled})
        self.world.wait_for(
            lambda: self.world.ws.call("GetStudioModeEnabled")["studioModeEnabled"] == enabled, timeout=5)

    def preview(self, scene):
        self.world.ws.call("SetCurrentPreviewScene", {"sceneName": scene})

    def state(self):
        return self.world.device_state(self.device_id, self.tmp_path / "state.json")["state"]

    def wait_for_lamps(self, red, green, timeout=5):
        self.world.wait_for(lambda: self.sim.lamps() == {"red": red, "green": green}, timeout=timeout)

    def cleanup(self):
        try:
            self.studio(False)
            self.world.ws.call("RemoveInput", {"inputName": SOURCE})
        except ObsWebSocketError:
            pass


def make_tally(request, obs_world, tmp_path, green=True, completions=True, **kwargs):
    sim = TallySim(green, completions)
    sim.start()
    request.addfinalizer(sim.stop)
    tally = Tally(obs_world, sim, tmp_path, **kwargs)
    request.addfinalizer(tally.cleanup)
    return tally


@pytest.fixture
def tally(request, obs_world, tmp_path):
    return make_tally(request, obs_world, tmp_path)


@pytest.fixture
def tally_without_green(request, obs_world, tmp_path):
    return make_tally(request, obs_world, tmp_path, green=False)


def test_red_lamp_is_lit_while_the_source_is_in_the_program_scene(tally):
    assert tally.sim.lamps() == {"red": False, "green": False}

    tally.program(tally.scene)
    tally.world.wait_for_device_source(tally.device_id, tally.tmp_path / "src.json", lambda r: r["live"], timeout=5)
    tally.wait_for_lamps(red=True, green=False)

    tally.program(tally.other)
    tally.wait_for_lamps(red=False, green=False)


def test_green_lamp_is_lit_while_the_source_is_in_the_preview_scene(obs_world, tally):
    tally.studio(True)

    tally.preview(tally.scene)
    tally.wait_for_lamps(red=False, green=True)

    tally.preview(tally.other)
    tally.wait_for_lamps(red=False, green=False)


def test_going_live_from_the_preview_swaps_the_green_lamp_for_the_red(obs_world, tally):
    tally.studio(True)
    tally.preview(tally.scene)
    tally.wait_for_lamps(red=False, green=True)

    # In both scenes, it is on air: only the red lamp
    tally.program(tally.scene)
    tally.wait_for_lamps(red=True, green=False)

    # Off the air again but still in the preview
    tally.program(tally.other)
    tally.wait_for_lamps(red=False, green=True)


def test_state_follows_the_lamps(obs_world, tally):
    tally.studio(True)
    tally.preview(tally.scene)
    obs_world.wait_for(lambda: tally.state().get("tally_preview") is True, timeout=5)
    assert tally.state().get("tally_on") in (None, False)

    tally.program(tally.scene)
    obs_world.wait_for(
        lambda: tally.state().get("tally_on") is True and tally.state().get("tally_preview") is False, timeout=5)


def test_lamps_can_be_requested(obs_world, tally):
    obs_world.run_ui_test("set_device_state", device_id=tally.device_id, tally_preview=True)
    tally.wait_for_lamps(red=False, green=True)
    obs_world.run_ui_test("set_device_state", device_id=tally.device_id, tally_on=True)
    tally.wait_for_lamps(red=True, green=True)

    obs_world.run_ui_test("set_device_state", device_id=tally.device_id, tally_preview=False, tally_on=False)
    tally.wait_for_lamps(red=False, green=False)


def test_a_camera_without_the_green_lamp_gets_the_red_one(obs_world, tally_without_green):
    tally = tally_without_green
    tally.studio(True)

    tally.preview(tally.scene)
    # The camera says it hasn't got that lamp, and that is that: no green
    # lamp is shown as lit
    assert tally.sim.lamps() == {"red": False, "green": False}
    assert not tally.state().get("tally_preview")

    tally.program(tally.scene)
    tally.wait_for_lamps(red=True, green=False)
    obs_world.wait_for(lambda: tally.state().get("tally_on") is True, timeout=5)
    assert not tally.state().get("tally_preview")


def test_no_lamps_are_lit_with_tally_auto_off(request, obs_world, tmp_path):
    tally = make_tally(request, obs_world, tmp_path, tally_auto=False)
    tally.studio(True)

    tally.preview(tally.scene)
    tally.program(tally.scene)
    # Nothing sent, so nothing to wait for: give it long enough that it
    # would have been
    time.sleep(1.5)
    assert tally.sim.lamps() == {"red": False, "green": False}


def test_state_follows_the_lamps_of_a_camera_that_never_completes_a_command(request, obs_world, tmp_path):
    tally = make_tally(request, obs_world, tmp_path, completions=False)
    tally.studio(True)

    tally.preview(tally.scene)
    obs_world.wait_for(lambda: tally.state().get("tally_preview") is True, timeout=5)
    tally.program(tally.scene)
    obs_world.wait_for(lambda: tally.state().get("tally_on") is True, timeout=5)
    tally.wait_for_lamps(red=True, green=False)
