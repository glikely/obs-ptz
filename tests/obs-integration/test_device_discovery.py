"""Covers detecting devices in the Add Device dialog: the drivers' ways of
finding cameras (src/ptz-discovery.hpp), and the dialog's "Detected devices"
section, which offers each device they find, to add as a filter set up to
reach it.

Two ways of finding cameras are covered, each against its own ptzsim:
ONVIF's WS-Discovery (src/ptz-onvif-discovery.cpp), which ptzsim's ONVIF
backend answers, and Sony's VISCA-over-IP setup protocol
(src/ptz-sony-discovery.cpp), which ptzsim answers with
--sony-discovery-name. Both are found over the network, multicast and
broadcast, so these need a network interface that can multicast and
broadcast; loopback alone won't do.

The dialog is driven through tests/ui-harness/device-backup-test.cpp's
"add_device", which waits for a detected device to show up before picking it.
"""

import json
import socket
import subprocess
import sys

import pytest
from obsws import ObsWebSocketError

from conftest import REPO_ROOT, free_port, free_udp_port, output_log, wait_for_port

SONY_VISCA_PORT = 52381


def primary_ipv4():
    """The address ptzsim advertises by default, and that answers probes"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 1))
        return s.getsockname()[0]
    finally:
        s.close()


class Sim:
    """A ptzsim of its own, started with `args`"""

    def __init__(self, name, args, wait_port):
        self.name = name
        self.args = args
        self.wait_port = wait_port
        self.proc = None

    def start(self):
        cmd = [sys.executable, "-m", "ptzsim", "--no-visca-serial", "--no-pelco"] + self.args
        with output_log(self.name) as out:
            self.proc = subprocess.Popen(cmd, cwd=REPO_ROOT / "scripts", stdout=out, stderr=subprocess.STDOUT)
        # A fixture that fails in setup isn't torn down: don't leave it running
        try:
            wait_for_port(primary_ipv4(), self.wait_port, timeout=15)
        except Exception:
            self.stop()
            raise

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
def onvif_sim(obs_world):
    port = free_port()
    sim = Sim("ptzsim-onvif-discovery", ["--no-visca", "--onvif-http-port", str(port)], port)
    sim.port = port
    sim.start()
    yield sim
    sim.stop()


@pytest.fixture
def sony_sim(obs_world):
    # Not on the port Sony's discovery says cameras use, 52381: a VISCA-over-IP
    # device binds its own end to its camera's port, so OBS can have it
    # already, on this same host, and the device made for this camera can't
    # reach it there
    sim = Sim("ptzsim-sony-discovery",
              ["--no-onvif", "--no-visca-tcp", "--visca-udp-port", str(free_udp_port()),
               "--sony-discovery-name", "SIMSONY1", "--debug-http-port", str(free_port())], 0)
    sim.wait_port = int(sim.args[-1])
    sim.start()
    yield sim
    sim.stop()


class Sources:
    """Empty media sources standing in for cameras, removed at the end"""

    def __init__(self, world):
        self.world = world
        self.scene = world.create_scene()
        self.names = set()

    def add(self, name):
        self.world.ws.call("CreateInput", {
            "sceneName": self.scene,
            "inputName": name,
            "inputKind": "ffmpeg_source",
            "inputSettings": {"is_local_file": True, "local_file": ""},
        })
        self.names.add(name)

    def filters(self, source):
        return self.world.ws.call("GetSourceFilterList", {"sourceName": source})["filters"]

    def wait_for_filters(self, source):
        """The filters once there are some: the dialog's result is written as
        it is accepted, a moment before the device is added to the source"""
        self.world.wait_for(lambda: self.filters(source))
        return self.filters(source)

    def cleanup(self):
        for name in list(self.names):
            try:
                self.world.ws.call("RemoveInput", {"inputName": name})
            except ObsWebSocketError:
                pass


@pytest.fixture
def sources(obs_world):
    s = Sources(obs_world)
    yield s
    s.cleanup()


def add_device(world, out, source, **choice):
    if out.exists():
        out.unlink()
    world.run_ui_test("add_device", filename=str(out), source=source, **choice)
    world.wait_for(out.exists, timeout=15)
    result = json.loads(out.read_text())
    items = result.get("choices", [])
    result["headings"] = [c["name"] for c in items if c["heading"]]
    result["detected"] = [c["name"] for c in items if c["detected"]]
    return result


def test_detected_onvif_camera_is_added(obs_world, sources, onvif_sim, tmp_path):
    host = primary_ipv4()
    sources.add("detect-onvif-cam")

    result = add_device(obs_world, tmp_path / "add.json", "detect-onvif-cam", detected=f":{onvif_sim.port}")

    assert "Detected devices" in result["headings"]
    assert result["chosen"] == f"obs-ptz-sim SIM-PTZ-1 ({host}:{onvif_sim.port})"
    filters = sources.wait_for_filters("detect-onvif-cam")
    assert [f["filterKind"] for f in filters] == ["ca.secretlab.obs-ptz.onvif"]
    settings = filters[0]["filterSettings"]
    assert (settings["type"], settings["host"], settings["port"]) == ("onvif", host, onvif_sim.port)
    device = obs_world.wait_for_device_by_name("detect-onvif-cam", tmp_path / "device.json",
                                               lambda r: r["found"] and r["bound"])
    obs_world.wait_for_device_status(device["uuid"], tmp_path / "status.json",
                                     lambda s: s["connected"] is True, timeout=15)


def test_detected_sony_camera_is_added(obs_world, sources, sony_sim, tmp_path):
    host = primary_ipv4()
    sources.add("detect-sony-cam")

    result = add_device(obs_world, tmp_path / "add.json", "detect-sony-cam", detected="SIMSONY1")

    assert result["chosen"] == f"SIMSONY1 SIM-PTZ-1 ({host})"
    filters = sources.wait_for_filters("detect-sony-cam")
    assert [f["filterKind"] for f in filters] == ["ca.secretlab.obs-ptz.visca"]
    settings = filters[0]["filterSettings"]
    assert (settings["type"], settings["host"], settings["udp_port"]) == ("visca-over-ip", host, SONY_VISCA_PORT)
    obs_world.wait_for_device_by_name("detect-sony-cam", tmp_path / "device.json",
                                      lambda r: r["found"] and r["bound"])


def test_a_camera_with_a_device_is_not_offered(obs_world, sources, sony_sim, tmp_path):
    sources.add("detect-sony-first")
    sources.add("detect-sony-second")
    add_device(obs_world, tmp_path / "add.json", "detect-sony-first", detected="SIMSONY1")
    obs_world.wait_for_device_by_name("detect-sony-first", tmp_path / "device.json",
                                      lambda r: r["found"] and r["bound"])

    result = add_device(obs_world, tmp_path / "add.json", "detect-sony-second", wait_search="true", cancel="true")

    assert not any("SIMSONY1" in d for d in result["detected"])
    assert sources.filters("detect-sony-second") == []


def test_detection_finishes(obs_world, sources, tmp_path):
    """Once the drivers have looked, the dialog says so: the note that it is
    searching goes, for "None found" if nothing answered"""
    sources.add("detect-none")
    result = add_device(obs_world, tmp_path / "add.json", "detect-none", wait_search="true", cancel="true")
    assert "Searching…" not in result["headings"]
    if not result["detected"]:
        assert "None found" in result["headings"]
