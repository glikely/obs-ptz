"""VISCA command sets: a camera is asked for what its model has.

A camera says what it is in its reply to the version inquiry, and PTZVisca
uses the command set (ViscaProfile, src/ptz-visca-commands.cpp) for that
model, or the generic one, which has everything and finds out what a camera
doesn't have from the syntax errors it answers with. The "visca_profile"
setting can name one instead.

A BirdDog P100 has none of the block inquiries, no green tally lamp and no
tally inquiry. ptzsim imitates one with --visca-model 0109:2020 and the
flags for each, and counts the requests it answers with a syntax error.
"""

import itertools
import json
import subprocess
import sys
import time
import urllib.request

import pytest
from obsws import ObsWebSocketError

from conftest import REPO_ROOT, free_port, output_log, wait_for_port

FILTER_KIND = "ca.secretlab.obs-ptz.visca"

# What a P100 has, and reads with its single-value inquiries
READ = {"power_on", "zoom_pos", "focus_pos", "focus_af_enabled", "wb_mode", "ae_mode", "r_gain"}

_sources = itertools.count(1)


class Sim:
    """A VISCA-over-TCP ptzsim that is a BirdDog P100, or with `model` and
    `flags`, another camera"""

    def __init__(self, model="0109:2020", flags=("--visca-no-block-inquiries", "--visca-no-green-tally"),
                 dvip=False):
        # with `dvip`, Datavideo's DVIP is on the TCP port instead
        self.tcp_port = free_port()
        self.debug_port = free_port()
        cmd = [
            sys.executable, "-m", "ptzsim",
            "--host", "127.0.0.1",
            *(("--visca-dvip-port", str(self.tcp_port), "--no-visca-tcp") if dvip else
              ("--visca-tcp-port", str(self.tcp_port))),
            "--visca-model", model, *flags,
            "--no-visca-udp", "--no-visca-serial", "--no-onvif", "--no-pelco",
            "--debug-http-port", str(self.debug_port),
        ]
        with output_log("ptzsim-p100") as out:
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

    def syntax_errors(self):
        return self.state()["visca_syntax_errors"]

    def wait_for(self, predicate, timeout=10):
        deadline = time.monotonic() + timeout
        while not predicate(state := self.state()):
            assert time.monotonic() < deadline, f"ptzsim never got there: {state}"
            time.sleep(0.1)
        return state


def camera(request, world, tmp_path, sim_args=None, read=READ, **settings):
    """A source with a VISCA filter pointed at a P100 of its own, or the
    camera `sim_args` makes a Sim: the sim, the state the device has read
    once it has read `read`, and the device's id"""
    sim = Sim(**(sim_args or {}))
    request.addfinalizer(sim.stop)
    source = f"profile-cam-{next(_sources)}"
    world.ws.call("CreateInput", {
        "sceneName": world.create_scene(),
        "inputName": source,
        "inputKind": "ffmpeg_source",
        "inputSettings": {"is_local_file": True, "local_file": ""},
    })

    def remove():
        try:
            world.ws.call("RemoveInput", {"inputName": source})
        except ObsWebSocketError:
            pass
    request.addfinalizer(remove)
    world.ws.call("CreateSourceFilter", {
        "sourceName": source,
        "filterName": "PTZ",
        "filterKind": FILTER_KIND,
        "filterSettings": {"type": "visca-over-tcp", "host": "127.0.0.1", "tcp_port": sim.tcp_port, **settings},
    })
    device_id = world.wait_for_device_by_name(
        source, tmp_path / "device.json", lambda r: r["found"] and r["bound"])["device_id"]
    state = world.wait_for_device_state(
        device_id, tmp_path / "state.json", lambda r: read <= set(r["state"]), timeout=15)["state"]
    return sim, state, device_id


def test_a_p100_is_only_asked_for_what_it_has(request, obs_world, tmp_path):
    sim, state, _ = camera(request, obs_world, tmp_path)
    assert (state["vendor_id"], state["model_id"]) == (0x0109, 0x2020)
    # long enough for the rest of what there is to read to have been
    time.sleep(2)
    assert sim.syntax_errors() == 0


def test_the_setting_chooses_the_command_set(request, obs_world, tmp_path):
    """The generic command set, for a P100, asks it for the block inquiries
    it doesn't have, and reads what they would have with the single-value
    ones"""
    sim, state, _ = camera(request, obs_world, tmp_path, visca_profile="generic")
    assert state["wb_mode"] is not None
    assert sim.syntax_errors() > 0


def test_a_sony_is_asked_for_what_its_manual_has(request, obs_world, tmp_path):
    """A camera that says it is a Sony SRG-120DH gets the command set from
    Sony's manual for it, which has no tally lamps, so it isn't asked for
    one, though ptzsim has them"""
    sim, state, device_id = camera(request, obs_world, tmp_path, sim_args={"model": "0001:0511", "flags": ()})
    assert (state["vendor_id"], state["model_id"]) == (0x0001, 0x0511)
    # long enough for the rest of what there is to read to have been
    time.sleep(2)
    assert "tally_on" not in obs_world.device_state(device_id, tmp_path / "later.json")["state"]


def test_a_birddog_is_read_with_its_own_block_inquiries(request, obs_world, tmp_path):
    """A BirdDog's command set reads where it is with BirdDog's own block
    inquiry for pan, tilt and zoom, which ptzsim has when it is one"""
    sim, state, device_id = camera(request, obs_world, tmp_path)
    obs_world.run_ui_test("move_device", device_id=device_id, mode="abs", zoom=0.5)
    obs_world.wait_for_device_state(device_id, tmp_path / "zoomed.json",
                                    lambda r: abs(r["state"].get("zoom", 0) - 0.5) < 0.01, timeout=10)
    assert sim.syntax_errors() == 0


def test_an_axis_gets_the_command_set_for_any_axis(request, obs_world, tmp_path):
    """An Axis camera says "AXV" and its product number where a Sony says its
    vendor and model IDs, so the Axis command set is for any camera with
    that vendor ID: one without presets, nor a focus position to read"""
    sim, state, device_id = camera(request, obs_world, tmp_path, sim_args={"model": "4158:5925", "flags": ()},
                                   read={"zoom_pos", "wb_mode", "features"})
    assert state["vendor_id"] == 0x4158
    assert "presets" not in state["features"]
    time.sleep(2)
    assert "focus_pos" not in obs_world.device_state(device_id, tmp_path / "later.json")["state"]


def moved_to(request, world, tmp_path, sim, device_id, pan, tilt):
    """Moves the camera to `pan` and `tilt`, and waits for it to say it's
    there; returns where ptzsim is, in its own units"""
    world.run_ui_test("move_device", device_id=device_id, mode="abs", pan=pan, tilt=tilt)
    world.wait_for_device_state(
        device_id, tmp_path / "moved.json",
        lambda r: abs(r["state"].get("pan", 9) - pan) < 0.01 and abs(r["state"].get("tilt", 9) - tilt) < 0.01,
        timeout=10)
    return sim.state()


SONY_SRG_300H = {"model": "0001:0513", "flags": ()}


def test_a_camera_moves_as_far_as_its_command_set_says(request, obs_world, tmp_path):
    """An SRG-300H tilts from -0x400 to 0x1200, further up than down, and
    pans to 0x2200 either way, which ptzsim's 0x2800 is beyond"""
    sim, _, device_id = camera(request, obs_world, tmp_path, sim_args=SONY_SRG_300H, read={"pan_pos"})
    at = moved_to(request, obs_world, tmp_path, sim, device_id, 0.5, 0.5)
    assert at["pan"] == pytest.approx(0x1100 / 0x2800, abs=0.002)
    assert at["tilt"] == pytest.approx(0x900 / 0x2800, abs=0.002)
    at = moved_to(request, obs_world, tmp_path, sim, device_id, -0.5, -0.5)
    assert at["tilt"] == pytest.approx(-0x200 / 0x2800, abs=0.002)


def test_the_settings_can_say_otherwise(request, obs_world, tmp_path):
    sim, _, device_id = camera(request, obs_world, tmp_path, sim_args=SONY_SRG_300H, read={"pan_pos"},
                               visca_ranges_auto=False, visca_pan_range=0x2800, visca_tilt_range=0x1400)
    at = moved_to(request, obs_world, tmp_path, sim, device_id, 0.5, -0.5)
    assert at["pan"] == pytest.approx(0.5, abs=0.002)
    assert at["tilt"] == pytest.approx(-0.25, abs=0.002)


def test_a_camera_with_20_bit_positions(request, obs_world, tmp_path):
    """An ILME-FR7's pan and tilt positions are 5 nibbles each, both in its
    moves and in its reply to the position inquiry"""
    sim, _, device_id = camera(request, obs_world, tmp_path, read={"pan_pos"}, sim_args={
        "model": "0001:051e", "flags": ("--visca-positions", "5:5", "--visca-pan-tilt-range", "0x9ca7")})
    at = moved_to(request, obs_world, tmp_path, sim, device_id, 0.5, 0.5)
    assert at["pan"] == pytest.approx(0.5, abs=0.002)
    assert at["tilt"] == pytest.approx(0xb3b0 / 2 / 0x9ca7, abs=0.002)


def test_a_camera_that_pans_right_to_left(request, obs_world, tmp_path):
    """A BRC-X1000's pan positions are 5 nibbles, and higher to the left;
    its tilt positions are 4"""
    sim, _, device_id = camera(request, obs_world, tmp_path, read={"pan_pos"}, sim_args={
        "model": "0001:0519", "flags": ("--visca-positions", "5:4", "--visca-pan-tilt-range", "0x9ca7")})
    at = moved_to(request, obs_world, tmp_path, sim, device_id, -0.5, 0.5)
    assert at["pan"] == pytest.approx(0.5, abs=0.002)
    assert at["tilt"] == pytest.approx(0x52ef / 2 / 0x9ca7, abs=0.002)


def test_a_camera_without_a_tilt_speed(request, obs_world, tmp_path):
    """A BRC-X400's moves have a 0 where the tilt speed would be"""
    sim, _, device_id = camera(request, obs_world, tmp_path, read={"pan_pos"},
                               sim_args={"model": "0001:051c", "flags": ()})
    at = moved_to(request, obs_world, tmp_path, sim, device_id, 0.5, 0.5)
    assert at["pan"] == pytest.approx(0x1100 / 0x2800, abs=0.002)


def test_a_birddog_in_standby_is_only_asked_whether_it_still_is(request, obs_world, tmp_path):
    """A BirdDog asked for anything but its camera details while in standby
    won't wake, Bitfocus' BirdDog PTZ Companion module says, and ptzsim
    imitates one that won't. The rest is read once it is on."""
    sim, state, device_id = camera(
        request, obs_world, tmp_path, read={"power_on"},
        sim_args={"model": "0109:2020",
                  "flags": ("--visca-no-block-inquiries", "--visca-no-green-tally", "--start-in-standby")})
    assert state["power_on"] is False
    # through a dozen of the reads of one inquiry a second, none of which
    # may go to a camera in standby
    time.sleep(12)
    assert "zoom_pos" not in obs_world.device_state(device_id, tmp_path / "standby.json")["state"]
    assert sim.state()["visca_standby_inquiries"] == 0
    obs_world.run_ui_test("set_device_state", device_id=device_id, power_on=True)
    sim.wait_for(lambda s: s["power"])
    obs_world.wait_for_device_state(device_id, tmp_path / "on.json", lambda r: READ <= set(r["state"]), timeout=15)


def test_a_shipped_command_set_can_have_a_value_the_generic_one_hasnt(request, obs_world, tmp_path):
    """PTZOptics' flicker reduction, which only its command set has"""
    sim, state, device_id = camera(request, obs_world, tmp_path, sim_args={"model": "0001:0000", "flags": ()},
                                   read={"flicker_mode"}, visca_profile="ptzoptics-gen2")
    assert state["flicker_mode"] == 0
    obs_world.run_ui_test("set_device_state", device_id=device_id, flicker_mode=2)
    sim.wait_for(lambda s: s["visca"]["flicker_mode"] == 2)


def test_a_datavideo_over_dvip(request, obs_world, tmp_path):
    """Datavideo's DVIP is VISCA over TCP with each packet's length before
    it, both ways"""
    sim, state, device_id = camera(request, obs_world, tmp_path, read={"zoom_pos", "wb_mode"},
                                   sim_args={"model": "0001:0000", "flags": (), "dvip": True},
                                   type="visca-over-dvip")
    assert state["description"].startswith("DVIP ")
    obs_world.run_ui_test("move_device", device_id=device_id, mode="abs", zoom=0.5)
    # the generic command set's zoom range, of ptzsim's
    sim.wait_for(lambda s: abs(s["zoom"] - 0x7ac0 / 2 / 0xe500) < 0.002)
