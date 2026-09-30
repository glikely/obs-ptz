"""VISCA camera settings: every value the Sony SRG-120DH manual has an inquiry
for is read into the device's state, every one it has a command for can be
asked for with ptz_request_state, and the settings dialog's state view shows
them and changes them (PTZVisca::inquires and requestState() in
src/ptz-visca.cpp, PTZStateView in src/ptz-state-view.cpp).

A camera with the "7e 7e xx" block inquiries has most of them read with
those; one without (a BirdDog, which ptzsim's --visca-no-block-inquiries
imitates) has each read with its single-value inquiry instead.

Every test here has a source with a VISCA filter pointed at a ptzsim of its
own, so that nothing else changes the camera's settings under it. The tests
that ask for a setting share one module-wide ptzsim, each asking for a
different setting, but each still has its own source: the scenes, and so the
sources and their devices, are all removed after every test. ptzsim reports
the settings in its debug state, under "visca", by the same keys as the
device's state.
"""

import itertools
import json
import subprocess
import sys
import urllib.request

import pytest
from obsws import ObsWebSocketError

from conftest import REPO_ROOT, free_port, output_log, wait_for_port

FILTER_KIND = "ca.secretlab.obs-ptz.visca"

# Each setting that can be asked for, and a value for it that isn't the one
# ptzsim starts with (see VISCA_SETTINGS in scripts/ptzsim/backends/visca.py)
SETTABLE = {
    "focus_af_mode": 1,
    "focus_af_sensitivity": False,
    "focus_af_move_time": 9,
    "focus_af_interval_time": 11,
    "focus_near_limit": 0x5000,
    "ir_correction": 1,
    "dzoom_on": True,
    "wb_mode": 5,
    "r_gain": 0x9a,
    "b_gain": 0x6b,
    "ae_mode": 0x3,
    "slow_shutter": True,
    "shutter_pos": 0x0d,
    "iris_pos": 0x08,
    "gain_pos": 0x05,
    "gain_limit": 0x0a,
    "bright_pos": 0x14,
    "exposure_comp": True,
    "exposure_comp_pos": 0x0a,
    "back_light": True,
    "wd_mode": 2,
    "defog_mode": True,
    "high_sensitivity": True,
    "aperture_gain": 0x0b,
    "high_resolution": True,
    "nr_level": 4,
    "gamma": 1,
    "chroma_suppress": 2,
    "color_gain": 0x0c,
    "color_hue": 0x03,
    "picture_effect": 4,
    "camera_id": 0x1234,
    "video_format": 0x03,
    "color_system": 2,
    "low_latency": True,
    "info_display": True,
    "ir_receive": False,
}

# What can only be read, with a single-value inquiry every camera here has
READ_ONLY = {
    "menu_on", "ir_condition", "pan_max_speed", "tilt_max_speed",
    "pantilt_init_status", "pantilt_move_status", "pantilt_pan_error", "pantilt_tilt_error",
    "pantilt_at_left_limit", "pantilt_at_right_limit", "pantilt_at_upper_limit", "pantilt_at_lower_limit",
}

# ...and what only a block inquiry, or the version inquiry, reads
READ_ONLY_BLOCKS = {
    "dzoom_pos", "low_contrast", "memory_recall_running", "focus_command_running", "zoom_command_running",
    "video_50hz", "vendor_name", "model_name", "rom_version", "socket_number",
}

_sources = itertools.count(1)


class CameraSim:
    """A VISCA-over-TCP ptzsim on its own ports, with or without the block
    inquiries"""

    def __init__(self, block_inquiries=True):
        self.block_inquiries = block_inquiries
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
        if not self.block_inquiries:
            cmd.append("--visca-no-block-inquiries")
        with output_log("ptzsim-camera-state") as out:
            self.proc = subprocess.Popen(cmd, cwd=REPO_ROOT / "scripts", stdout=out, stderr=subprocess.STDOUT)
        wait_for_port("127.0.0.1", self.debug_port, timeout=15)
        wait_for_port("127.0.0.1", self.tcp_port, timeout=15)

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()

    def settings(self):
        with urllib.request.urlopen(f"http://127.0.0.1:{self.debug_port}/state", timeout=5) as resp:
            return json.load(resp)["visca"]


class Camera:
    """A source with a VISCA filter pointed at `sim`, and its device"""

    def __init__(self, world, sim, tmp_path):
        self.world = world
        self.sim = sim
        self.tmp_path = tmp_path
        self.source = f"camera-state-cam-{next(_sources)}"
        world.ws.call("CreateInput", {
            "sceneName": world.create_scene(),
            "inputName": self.source,
            "inputKind": "ffmpeg_source",
            "inputSettings": {"is_local_file": True, "local_file": ""},
        })
        world.ws.call("CreateSourceFilter", {
            "sourceName": self.source,
            "filterName": "PTZ",
            "filterKind": FILTER_KIND,
            "filterSettings": {"type": "visca-over-tcp", "host": "127.0.0.1", "tcp_port": sim.tcp_port},
        })
        self.device_id = world.wait_for_device_by_name(
            self.source, tmp_path / "device.json", lambda r: r["found"] and r["bound"])["device_id"]

    def wait_for_state(self, predicate, timeout=15):
        return self.world.wait_for_device_state(
            self.device_id, self.tmp_path / "state.json", lambda r: predicate(r["state"]), timeout=timeout)["state"]

    def cleanup(self):
        try:
            self.world.ws.call("RemoveInput", {"inputName": self.source})
        except ObsWebSocketError:
            pass


def make_camera(finalizer, world, tmp_path, block_inquiries=True):
    sim = CameraSim(block_inquiries)
    sim.start()
    finalizer(sim.stop)
    camera = Camera(world, sim, tmp_path)
    finalizer(camera.cleanup)
    return camera


@pytest.fixture(scope="module")
def shared_sim(request, obs_world):
    """One ptzsim for the tests that ask for settings, each a different one"""
    sim = CameraSim()
    sim.start()
    request.addfinalizer(sim.stop)
    return sim


@pytest.fixture
def camera(request, obs_world, shared_sim, tmp_path):
    camera = Camera(obs_world, shared_sim, tmp_path)
    request.addfinalizer(camera.cleanup)
    return camera


@pytest.mark.parametrize("block_inquiries", [True, False], ids=["block-inquiries", "single-inquiries"])
def test_every_setting_is_read(request, obs_world, tmp_path, block_inquiries):
    camera = make_camera(request.addfinalizer, obs_world, tmp_path, block_inquiries)
    expected = set(SETTABLE) | READ_ONLY | (READ_ONLY_BLOCKS if block_inquiries else set())
    state = camera.wait_for_state(lambda s: expected <= set(s))

    # As the camera has them: nothing has changed them since it started
    settings = camera.sim.settings()
    assert {key: state[key] for key in SETTABLE} == {key: settings[key] for key in SETTABLE}
    assert state["pantilt_init_status"] == 2  # done
    if block_inquiries:
        assert state["vendor_name"] == "Sony"
        assert state["video_50hz"] is (settings["video_format"] >= 0x08)


@pytest.mark.parametrize("key", SETTABLE)
def test_a_setting_can_be_asked_for(obs_world, camera, key):
    value = SETTABLE[key]
    # read, so that the device is connected
    camera.wait_for_state(lambda s: key in s)
    obs_world.run_ui_test("set_device_state", device_id=camera.device_id, **{key: value})

    obs_world.wait_for(lambda: camera.sim.settings()[key] == value, timeout=5)
    # and is read back from the camera
    camera.wait_for_state(lambda s: s.get(key) == value, timeout=5)


def test_one_af_time_asked_for_keeps_the_other(request, obs_world, tmp_path):
    """Both are set by one command"""
    camera = make_camera(request.addfinalizer, obs_world, tmp_path)
    before = camera.wait_for_state(lambda s: "focus_af_move_time" in s)["focus_af_move_time"]

    obs_world.run_ui_test("set_device_state", device_id=camera.device_id, focus_af_interval_time=21)
    obs_world.wait_for(lambda: camera.sim.settings()["focus_af_interval_time"] == 21, timeout=5)
    assert camera.sim.settings()["focus_af_move_time"] == before


def test_the_menu_is_not_opened(request, obs_world, tmp_path):
    """VISCA can only close the camera's on-screen menu"""
    camera = make_camera(request.addfinalizer, obs_world, tmp_path)
    camera.wait_for_state(lambda s: s.get("menu_on") is False)

    obs_world.run_ui_test("set_device_state", device_id=camera.device_id, menu_on=True, info_display=True)
    obs_world.wait_for(lambda: camera.sim.settings()["info_display"] is True, timeout=5)
    assert camera.sim.settings()["menu_on"] is False


# The state view: a field of each kind, and a value to pick in it that isn't
# the one ptzsim starts with
DIALOG_EDITS = {
    "back_light": True,             # a checkbox
    "ae_mode": 0xb,                 # a list
    "focus_af_sensitivity": False,  # a list of two, for a bool
    "r_gain": 0x33,                 # a number
    "camera_id": 0xbeef,            # a number, in hex
}


def open_dialog(obs_world, camera, tmp_path):
    obs_world.run_ui_test("open_settings_dialog", device_id=camera.device_id)
    out = tmp_path / "dialog.json"
    return out, obs_world.wait_for_settings_dialog(
        out, lambda r: set(SETTABLE) | READ_ONLY | READ_ONLY_BLOCKS <= r["state_keys"], timeout=15)


def test_state_view_shows_every_setting(request, obs_world, tmp_path):
    camera = make_camera(request.addfinalizer, obs_world, tmp_path)
    state = camera.wait_for_state(lambda s: set(SETTABLE) <= set(s))

    _, dialog = open_dialog(obs_world, camera, tmp_path)

    assert {key: dialog["shown"][key] for key in SETTABLE} == {key: state[key] for key in SETTABLE}
    # read-only values are shown as text, named where they have names
    assert dialog["shown"]["vendor_name"] == "Sony (0x0001)"
    assert dialog["shown"]["pantilt_init_status"] == "Done"


@pytest.mark.parametrize("key", DIALOG_EDITS)
def test_state_view_changes_a_setting(request, obs_world, tmp_path, key):
    camera = make_camera(request.addfinalizer, obs_world, tmp_path)
    value = DIALOG_EDITS[key]
    out, _ = open_dialog(obs_world, camera, tmp_path)

    obs_world.run_ui_test("edit_dialog_state", **{key: value})

    obs_world.wait_for(lambda: camera.sim.settings()[key] == value, timeout=5)
    # and the view follows the camera to it
    obs_world.wait_for_settings_dialog(out, lambda r: r["shown"].get(key) == value, timeout=5)


def test_state_view_has_none_of_them_for_another_protocol(obs_world, tmp_path):
    obs_world.run_ui_test("open_settings_dialog", device_id=obs_world.device_ids["pelco-d"])
    dialog = obs_world.wait_for_settings_dialog(tmp_path / "dialog.json", lambda r: "name" in r["state_keys"])
    assert not (set(SETTABLE) | READ_ONLY | READ_ONLY_BLOCKS) & dialog["state_keys"]
