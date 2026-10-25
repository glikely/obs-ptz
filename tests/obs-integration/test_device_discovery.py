"""Covers detecting devices: each driver's discovery kind, a hidden OBS
source kind (src/core/ptz-discovery-kind.hpp, "Discovery kinds" in
docs/ptz-device-api.md) driven only through its instance's own procs and
signals, and the Add Device dialog's "Detected devices" section, which makes
one instance of each, and offers each device they find, to add as a filter
set up to reach it.

Two ways of finding cameras are covered, each against its own ptzsim:
ONVIF's WS-Discovery (src/drivers/onvif/ptz-onvif-discovery.cpp), which ptzsim's ONVIF
backend answers, and Sony's VISCA-over-IP setup protocol
(src/drivers/visca/ptz-sony-discovery.cpp), which ptzsim answers with
--sony-discovery-name. Both are found over the network, multicast and
broadcast, so these need a network interface that can multicast and
broadcast; loopback alone won't do.

The dialog is driven through the UI driver (World.add_device() in conftest.py),
which waits for a detected device to show up before picking it.
The discovery kinds, without the dialog, are driven over the PTZ API's
websocket binding, as a remote controller does, and, for what only a listener
inside OBS can do from inside a signal, through
tests/ui-harness/discovery-test.cpp.
"""

import json
import socket
import subprocess
import sys
import time

import pytest
from obsws import ObsWebSocketError

from conftest import REPO_ROOT, free_port, free_udp_port, output_log, wait_for_port
from test_api_version import declared_version

SONY_VISCA_PORT = 52381

PREFIX = "ca.secretlab.obs-ptz."
DISCOVERY_KINDS = {PREFIX + "visca.discovery", PREFIX + "onvif.discovery"}


def primary_ipv4():
    """The address ptzsim advertises by default, and that answers probes"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 1))
        return s.getsockname()[0]
    finally:
        s.close()


def require_network():
    """Skips the test where there is no network interface to be found over, as
    on a computer with no connection: it is the network that finds the camera, and
    a ptzsim that is only on loopback answers nothing"""
    try:
        address = primary_ipv4()
    except OSError:
        address = None
    if not address or address.startswith("127."):
        pytest.skip("no network interface to detect a camera over: discovery needs one that is not loopback")


class Sim:
    """A ptzsim of its own, started with `args`"""

    def __init__(self, name, args, wait_port):
        self.name = name
        self.args = args
        self.wait_port = wait_port
        self.proc = None

    def start(self):
        # Discovery is answered over the network, so it needs the address ptzsim says it is at
        cmd = [sys.executable, "-m", "ptzsim", "--no-visca-serial", "--no-pelco", "--host", primary_ipv4()] + self.args
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
    require_network()
    port = free_port()
    sim = Sim("ptzsim-onvif-discovery", ["--no-visca", "--onvif-http-port", str(port)], port)
    sim.port = port
    sim.start()
    yield sim
    sim.stop()


@pytest.fixture
def onvif_sim_two_matches(obs_world):
    """An ONVIF ptzsim that answers each probe with two ProbeMatches in the one
    reply, so the driver reports twice from one datagram"""
    require_network()
    port = free_port()
    sim = Sim("ptzsim-onvif-two-matches",
              ["--no-visca", "--onvif-http-port", str(port), "--onvif-probe-matches", "2"], port)
    sim.port = port
    sim.start()
    yield sim
    sim.stop()


@pytest.fixture
def sony_sim(obs_world):
    require_network()
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


# --- the discovery kinds, without the dialog ----------------------------------------
# First, so that a dialog test that leaves its dialog open (it is modal, and the
# harness answers it from inside its event loop) doesn't take these with it


def discovery_kinds(world):
    """The kinds of the discovery instances the websocket binding makes: one for each
    discovery kind OBS has, as the controller finds them"""
    return {i["kind"] for i in world.ws.vendor("obs-ptz", "ptz_list_discovery")["instances"]}


def discover(world, out, **mode):
    """Makes a private instance of each discovery kind, starts it, and has what
    each said until it finished (see tests/ui-harness/discovery-test.cpp for
    `mode`): what only a listener inside OBS can do, from inside a signal"""
    world.run_ui_test("discover", filename=str(out), **mode)
    world.wait_for(out.exists, timeout=20)
    return json.loads(out.read_text())


def assert_responsive(world):
    """The main thread still runs requests, promptly: the UI driver does its on it"""
    world.need_ui_driver()
    world.ui.call("ping")
    assert discovery_kinds(world) == DISCOVERY_KINDS


def test_discovery_kind_not_listed(obs_world):
    """OBS doesn't offer a discovery kind as a source to add, while the
    controller still finds it"""
    listed = set(obs_world.ws.call("GetInputKindList", {"unversioned": False})["inputKinds"])
    listed |= set(obs_world.ws.call("GetInputKindList", {"unversioned": True})["inputKinds"])
    assert listed, "OBS listed no input kinds at all"
    assert DISCOVERY_KINDS & listed == set()

    assert discovery_kinds(obs_world) == DISCOVERY_KINDS


def discover_over_websocket(world, timeout=20):
    """Starts each of the websocket binding's discovery instances, as a remote
    controller does, and has what each said until it finished, and for a while
    after: {"instances": {uuid: kind}, "said": [(event, data)...]}"""
    instances = {i["device"]: i["kind"] for i in world.ws.vendor("obs-ptz", "ptz_list_discovery")["instances"]}
    # The binding keeps its instances: one may still be looking for an earlier test
    for uuid in instances:
        world.ws.vendor("obs-ptz", "ptz_discovery_stop", {"device": uuid})
    world.events.events("VendorEvent", settle=0.3)
    for uuid in instances:
        world.ws.vendor("obs-ptz", "ptz_discovery_start", {"device": uuid})

    said = []
    finished = set()

    def listen(settle):
        for event in world.events.events("VendorEvent", settle=settle):
            data = event.get("eventData") or {}
            if event.get("vendorName") != "obs-ptz" or data.get("device") not in instances:
                continue
            said.append((event["eventType"], data))
            if event["eventType"] == "ptz_discovery_finished":
                finished.add(data["device"])

    deadline = time.time() + timeout
    while finished != set(instances) and time.time() < deadline:
        listen(0.5)
    # Anything said after finishing is said soon after
    listen(1.0)
    return {"instances": instances, "said": said}


def test_discovery_through_websocket(obs_world, onvif_sim, sony_sim):
    host = primary_ipv4()
    result = discover_over_websocket(obs_world)

    instances = result["instances"]
    assert set(instances.values()) == DISCOVERY_KINDS
    for uuid, kind in instances.items():
        version = obs_world.ws.vendor("obs-ptz", "ptz_get_api_version", {"device": uuid})
        assert (version["major"], version["minor"]) == declared_version(), kind
        said = [event for event, data in result["said"] if data["device"] == uuid]
        assert said.count("ptz_discovery_finished") == 1, kind
        assert "ptz_device_found" not in said[said.index("ptz_discovery_finished"):], f"{kind} found after finishing"

    found = [{**data, "discovery": instances[data["device"]]}
             for event, data in result["said"] if event == "ptz_device_found"]
    for device in found:
        assert device["kind"] == device["discovery"].removesuffix(".discovery"), device
    for kind in DISCOVERY_KINDS:
        ids = [d["id"] for d in found if d["discovery"] == kind]
        assert len(ids) == len(set(ids)), f"{kind} found one twice"

    sony = [d for d in found if "SIMSONY1" in d["name"]]
    assert len(sony) == 1, found
    assert sony[0]["discovery"] == PREFIX + "visca.discovery"
    assert sony[0]["name"] == "SIMSONY1 SIM-PTZ-1"
    assert sony[0]["address"] == host
    assert sony[0]["settings"] == {"type": "visca-over-ip", "host": host, "udp_port": SONY_VISCA_PORT}

    onvif = [d for d in found if d["address"] == f"{host}:{onvif_sim.port}"]
    assert len(onvif) == 1, found
    assert onvif[0]["discovery"] == PREFIX + "onvif.discovery"
    assert onvif[0]["name"] == "obs-ptz-sim SIM-PTZ-1"
    assert onvif[0]["settings"] == {"type": "onvif", "host": host, "port": onvif_sim.port}


def test_stop_from_inside_found_is_heard(obs_world, sony_sim, onvif_sim_two_matches, tmp_path):
    """A controller may stop an instance from inside its ptz_device_found: the
    driver says it has finished, on the same thread, inside that signal, and
    says nothing more, not even for the other ProbeMatch of the same reply"""
    result = discover(obs_world, tmp_path / "discover.json", stop_on_found="true")

    for kind in DISCOVERY_KINDS:
        assert result["instances"][kind]["stopped"], f"{kind} found nothing to stop from inside"
    for kind, instance in result["instances"].items():
        assert instance["finished"] == 1, kind
        assert instance["found_after_stop"] == 0, kind
    assert_responsive(obs_world)


def test_start_from_inside_finished_looks_again(obs_world, sony_sim, tmp_path):
    """A controller may start an instance again from inside its
    ptz_discovery_finished: it looks again, and finishes again"""
    result = discover(obs_world, tmp_path / "discover.json", restart_on_finished="true")

    for kind, instance in result["instances"].items():
        assert instance["finished"] == 2, kind
    sony = [d for d in result["found"] if "SIMSONY1" in d["name"]]
    assert len(sony) == 2, "each search finds the camera once"
    assert_responsive(obs_world)


def test_release_from_inside_found_is_safe(obs_world, onvif_sim_two_matches, tmp_path):
    """A controller may let go of an instance's last reference from inside its
    ptz_device_found: the driver, which then still has the reply's other
    ProbeMatch to report, says nothing more, and OBS carries on"""
    result = discover(obs_world, tmp_path / "discover.json", release_on_found="true")

    onvif = result["instances"][PREFIX + "onvif.discovery"]
    assert onvif["released"], "found nothing to let go from inside"
    for kind, instance in result["instances"].items():
        assert instance["found_after_release"] == 0, kind
        assert instance["finished_after_release"] == 0, kind
    ids = [d["id"] for d in result["found"] if d["discovery"] == PREFIX + "onvif.discovery"]
    assert len(ids) == 1, "the second ProbeMatch was reported after the release"
    assert_responsive(obs_world)


def test_releasing_a_running_instance_silences_it(obs_world, tmp_path):
    """Released while it is still looking, without ptz_discovery_stop, an
    instance is destroyed and says nothing once OBS destroys it, nor when its
    driver's own search would have run its course (3 s).

    This can't tell the destroy's detach() of the reporter from its
    deleteLater() of the driver: either alone keeps the driver quiet here."""
    result = discover(obs_world, tmp_path / "discover.json", release_while_running="true", wait_ms=4000)

    assert result["instances"] == len(DISCOVERY_KINDS)
    assert result["destroyed"] == result["instances"]
    # A camera that answers between the release and OBS getting round to
    # destroying it may still be found; finishing would take the 3 s search
    assert result["finished_after_release"] == 0
    assert result["found_after_destroy"] == 0
    assert result["finished_after_destroy"] == 0
    assert_responsive(obs_world)


# --- the Add Device dialog ---------------------------------------------------------


def add_device(world, out, source, **choice):
    result = world.add_device(source, **choice)
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


def discovery_spy(world, out):
    if out.exists():
        out.unlink()
    world.run_ui_test("get_discovery_spy", filename=str(out))
    world.wait_for(out.exists)
    return json.loads(out.read_text())


def test_closing_dialog_stops_search(obs_world, sources, sony_sim, tmp_path):
    """Closing the dialog while its drivers are still looking destroys their
    instances, which say nothing afterwards, not even that they finished, and
    opening it again looks again"""
    sources.add("detect-close")

    result = add_device(obs_world, tmp_path / "add.json", "detect-close", cancel="true", spy_discovery="true")
    assert "Searching…" in result["headings"], "closed after the search finished: nothing to stop"

    spy = discovery_spy(obs_world, tmp_path / "spy.json")
    assert spy["closed"]
    assert spy["instances"] == len(DISCOVERY_KINDS)
    # Each instance looks for 3 s: wait longer than that, for one that wasn't stopped
    obs_world.wait_for(lambda: discovery_spy(obs_world, tmp_path / "spy.json")["destroyed"] == spy["instances"],
                       timeout=5)
    time.sleep(3.5)
    spy = discovery_spy(obs_world, tmp_path / "spy.json")
    assert spy["destroyed"] == spy["instances"]
    assert spy["alive"] == 0
    assert spy["found_after_close"] == 0
    assert spy["finished_after_close"] == 0

    result = add_device(obs_world, tmp_path / "add.json", "detect-close", detected="SIMSONY1", cancel="true")
    assert any("SIMSONY1" in d for d in result["detected"]), result
