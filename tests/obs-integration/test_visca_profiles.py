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

    def __init__(self, model="0109:2020", flags=("--visca-no-block-inquiries", "--visca-no-green-tally")):
        self.tcp_port = free_port()
        self.debug_port = free_port()
        cmd = [
            sys.executable, "-m", "ptzsim",
            "--host", "127.0.0.1",
            "--visca-tcp-port", str(self.tcp_port),
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
    device_name = world.wait_for_device_by_name(
        source, tmp_path / "device.json", lambda r: r["found"] and r["bound"])["uuid"]
    state = world.wait_for_device_state(
        device_name, tmp_path / "state.json", lambda r: read <= set(r["state"]), timeout=15)["state"]
    return sim, state, device_name


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
    sim, state, device_name = camera(request, obs_world, tmp_path, sim_args={"model": "0001:0511", "flags": ()})
    assert (state["vendor_id"], state["model_id"]) == (0x0001, 0x0511)
    # long enough for the rest of what there is to read to have been
    time.sleep(2)
    assert "tally_on" not in obs_world.device_state(device_name, tmp_path / "later.json")["state"]
