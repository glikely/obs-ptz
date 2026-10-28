"""Covers a move of pan or tilt alone, through the ptz_move, ptz_move_rel and
ptz_move_abs procs (PTZDevice::move()/move_rel()/move_abs() in
src/core/ptz-device.cpp): each is an axis of its own, so the other is left as it
is. A drive leaves the other at its speed, a relative move leaves it where it
is, and an absolute move, which a camera takes pan and tilt together for, sends
the other where the device's state says it is now (PTZDevice::pairPanTilt()),
or neither if that isn't known.

Driven over obs-websocket against the shared ptzsim, whose own debug state has
the speed and position each backend set.
"""

import time

import pytest
from conftest import free_udp_port

VISCA_BACKENDS = ["visca-tcp", "visca-udp", "visca-serial"]
# The ones that go to a position, and move by a distance
POSITION_BACKENDS = VISCA_BACKENDS + ["onvif"]
DRIVE_BACKENDS = POSITION_BACKENDS + ["pelco-d"]


def near(value, target, within=0.02):
    return value is not None and abs(value - target) < within


def go_to(world, device, out, pan, tilt):
    """Moves device to pan and tilt, and waits for its state to say it is there,
    which is what a lone axis is sent with"""
    world.move_device(device, mode="abs", pan=pan, tilt=tilt)
    world.wait_for_device_state(
        device, out, lambda r: near(r["state"].get("pan"), pan) and near(r["state"].get("tilt"), tilt), timeout=15)
    return world.state()


@pytest.mark.parametrize("backend", POSITION_BACKENDS)
def test_absolute_pan_alone_leaves_tilt(obs_world, backend, tmp_path):
    device = obs_world.device_names[backend]
    out = tmp_path / "state.json"
    before = go_to(obs_world, device, out, 0.0, -0.5)

    obs_world.move_device(device, mode="abs", pan=0.5)

    obs_world.wait_for_device_state(device, out, lambda r: near(r["state"].get("pan"), 0.5), timeout=15)
    assert near(obs_world.device_state(device, out)["state"]["tilt"], -0.5)
    assert near(obs_world.state()["tilt"], before["tilt"], 0.005)


@pytest.mark.parametrize("backend", POSITION_BACKENDS)
def test_absolute_tilt_alone_leaves_pan(obs_world, backend, tmp_path):
    device = obs_world.device_names[backend]
    out = tmp_path / "state.json"
    before = go_to(obs_world, device, out, 0.5, 0.0)

    obs_world.move_device(device, mode="abs", tilt=-0.5)

    obs_world.wait_for_device_state(device, out, lambda r: near(r["state"].get("tilt"), -0.5), timeout=15)
    assert near(obs_world.device_state(device, out)["state"]["pan"], 0.5)
    assert near(obs_world.state()["pan"], before["pan"], 0.005)


@pytest.mark.parametrize("backend", POSITION_BACKENDS)
def test_relative_pan_alone_leaves_tilt(obs_world, backend, tmp_path):
    device = obs_world.device_names[backend]
    out = tmp_path / "state.json"
    before = go_to(obs_world, device, out, 0.0, -0.5)

    obs_world.move_device(device, mode="rel", pan=0.1)

    obs_world.wait_for_device_state(device, out, lambda r: r["state"]["pan"] > 0.05, timeout=15)
    assert near(obs_world.device_state(device, out)["state"]["tilt"], -0.5)
    assert near(obs_world.state()["tilt"], before["tilt"], 0.005)


@pytest.mark.parametrize("backend", DRIVE_BACKENDS)
def test_driving_tilt_alone_leaves_pan_moving(obs_world, backend):
    device = obs_world.device_names[backend]
    try:
        obs_world.call_proc(device, "ptz_move", {"pan": 0.3, "tilt": 0.3})
        obs_world.wait_for_state(lambda s: s["pan_speed"] != 0 and s["tilt_speed"] != 0, timeout=5)

        obs_world.call_proc(device, "ptz_move", {"tilt": 0.0})
        obs_world.wait_for_state(lambda s: s["tilt_speed"] == 0, timeout=5)
        # and not stopped a moment after, either
        time.sleep(0.5)
        assert obs_world.state()["pan_speed"] != 0
    finally:
        obs_world.call_proc(device, "ptz_stop")
        obs_world.wait_for_state(lambda s: s["pan_speed"] == 0 and s["tilt_speed"] == 0, timeout=5)


@pytest.mark.parametrize("backend", DRIVE_BACKENDS)
@pytest.mark.parametrize("other", ["zoom", "focus"])
def test_driving_pan_leaves_the_lens_moving(obs_world, backend, other):
    """Pelco's command word says how every axis moves, so one that leaves an
    axis out stops it"""
    if backend == "onvif" and other == "focus":
        pytest.skip("ptzsim's ONVIF has no imaging service to focus with")
    device = obs_world.device_names[backend]
    speed = f"{other}_speed"
    try:
        obs_world.call_proc(device, "ptz_move", {other: 0.5})
        obs_world.wait_for_state(lambda s: s[speed] != 0, timeout=5)

        obs_world.call_proc(device, "ptz_move", {"pan": 0.3})
        obs_world.wait_for_state(lambda s: s["pan_speed"] != 0, timeout=5)
        time.sleep(0.5)
        assert obs_world.state()[speed] != 0
    finally:
        obs_world.call_proc(device, "ptz_stop")
        obs_world.wait_for_state(lambda s: s["pan_speed"] == 0 and s[speed] == 0, timeout=5)


@pytest.fixture
def unconnected_camera(obs_world):
    """A VISCA device whose camera never answers, so its state never says where
    the camera is"""
    scene = obs_world.create_scene()
    name = "lone-axis-unconnected"
    obs_world.ws.call("CreateInput", {
        "sceneName": scene,
        "inputName": name,
        "inputKind": "ffmpeg_source",
        "inputSettings": {"is_local_file": True, "local_file": ""},
    })
    obs_world.ws.call("CreateSourceFilter", {
        "sourceName": name,
        "filterName": "PTZ",
        "filterKind": "ca.secretlab.obs-ptz.visca",
        "filterSettings": {"type": "visca-over-ip", "host": "127.0.0.1", "udp_port": free_udp_port()},
    })
    obs_world.wait_for(lambda: obs_world.device_uuid(name) is not None, timeout=5)
    yield name
    obs_world.ws.call("RemoveInput", {"inputName": name})


def test_absolute_pan_alone_where_tilt_is_not_known_moves_nothing(obs_world, unconnected_camera, tmp_path):
    state = obs_world.device_state(unconnected_camera, tmp_path / "state.json")["state"]
    assert "pan" not in state and "tilt" not in state
    log_before = len(obs_world.obs_log().read_text(errors="replace"))

    obs_world.move_device(unconnected_camera, mode="abs", pan=0.5)

    obs_world.wait_for(lambda: "not moving pan alone: where the camera is not known"
                       in obs_world.obs_log().read_text(errors="replace")[log_before:], timeout=5)
