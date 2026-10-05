"""Whether a VISCA camera is powered is read before the rest of its state:
PTZVisca::send_pending() in src/ptz-visca.cpp.

When the link comes up everything the camera can be asked for is read, one
inquiry at a time. The device list shows a power indicator for a camera in
standby, so power_on must not wait its turn behind the others: it was read
in whatever order a set of keys came out in, which put it last often enough
that the indicator was missing for seconds after startup.

Only vendor_id (the version inquiry, which has to come first to choose the
camera's command set) may come before it.

Each test has its own ptzsim, in standby, that records the inquiries it was
sent in order (see "visca_inquiries" in ptzsim's debug state).
"""

import json
import subprocess
import sys
import urllib.request

import pytest
from obsws import ObsWebSocketError

from conftest import REPO_ROOT, free_port, output_log, wait_for_port

FILTER_KIND = "ca.secretlab.obs-ptz.visca"
SOURCE = "power-first-cam"

VERSION_INQUIRY = "81090002"
POWER_INQUIRY = "81090400"


class StandbySim:
    def __init__(self):
        self.tcp_port = free_port()
        self.debug_port = free_port()
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
        with output_log("ptzsim-power-first") as out:
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


@pytest.fixture
def camera(request, obs_world, tmp_path):
    """A camera in standby, with a source with a VISCA filter on it, that
    OBS has connected to"""
    sim = StandbySim()
    sim.start()
    request.addfinalizer(sim.stop)

    scene = obs_world.create_scene()
    obs_world.ws.call("CreateInput", {
        "sceneName": scene,
        "inputName": SOURCE,
        "inputKind": "ffmpeg_source",
        "inputSettings": {"is_local_file": True, "local_file": ""},
    })

    def remove_source():
        try:
            obs_world.ws.call("RemoveInput", {"inputName": SOURCE})
        except ObsWebSocketError:
            pass

    request.addfinalizer(remove_source)
    obs_world.ws.call("CreateSourceFilter", {
        "sourceName": SOURCE,
        "filterName": "PTZ",
        "filterKind": FILTER_KIND,
        "filterSettings": {"type": "visca-over-tcp", "host": "127.0.0.1", "tcp_port": sim.tcp_port},
    })
    device_name = obs_world.wait_for_device_by_name(
        SOURCE, tmp_path / "device.json", lambda r: r["found"] and r["bound"])["uuid"]
    obs_world.wait_for_device_status(device_name, tmp_path / "status.json", lambda s: s["connected"], timeout=10)
    return sim, device_name


def test_power_is_asked_for_straight_after_the_version(obs_world, camera, tmp_path):
    sim, device_name = camera
    obs_world.wait_for_device_state(device_name, tmp_path / "state.json", lambda r: "power_on" in r["state"], timeout=15)

    inquiries = [inquiry[:8] for inquiry in sim.state()["visca_inquiries"]]
    assert POWER_INQUIRY in inquiries, inquiries
    # What was asked for before it can only be the version
    before = inquiries[:inquiries.index(POWER_INQUIRY)]
    assert set(before) <= {VERSION_INQUIRY}, inquiries


def test_a_camera_in_standby_is_known_to_be_off(obs_world, camera, tmp_path):
    _, device_name = camera
    state = obs_world.wait_for_device_state(
        device_name, tmp_path / "state.json", lambda r: "power_on" in r["state"], timeout=15)["state"]
    assert state["power_on"] is False
