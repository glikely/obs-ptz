"""Covers the PTZ API over obs-websocket ("WebSocket binding" in
docs/ptz-device-api.md, src/controller/ptz-websocket.cpp): the "obs-ptz"
vendor's requests, one for each proc of a device, as a client outside OBS
calls them, what it is told when it asks wrongly, and the setting that says
how much of it a client is let do.

Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
SPDX-License-Identifier: GPL-2.0-or-later

Nothing here goes through the UI test harness but the changing of the
setting, which only the settings dialog does otherwise.
"""

import json

import pytest
from obsws import ObsWebSocketError

VENDOR = "obs-ptz"
# obs-websocket's RequestStatus::ResourceNotFound, which it answers for a
# vendor request that nothing has registered
NOT_FOUND = 600


def devices(obs_world):
    """{name of the source a device is on: its entry in ptz_list_devices}"""
    return {d["source"]: d for d in obs_world.ws.vendor(VENDOR, "ptz_list_devices")["devices"]}


def device(obs_world, backend):
    return devices(obs_world)[obs_world.device_names[backend]]["device"]


def error_of(obs_world, request, data):
    with pytest.raises(ObsWebSocketError) as raised:
        obs_world.ws.vendor(VENDOR, request, data)
    return raised.value.status.get("error")


def is_unknown_request(obs_world, request, data=None):
    """Whether obs-websocket has no such request of the vendor's"""
    try:
        obs_world.ws.vendor(VENDOR, request, data)
    except ObsWebSocketError as e:
        return e.status.get("code") == NOT_FOUND and "error" not in e.status
    return False


def test_bridge_version(obs_world):
    assert obs_world.ws.vendor(VENDOR, "ptz_bridge_version") == {"major": 1, "minor": 0}


def test_list_devices_has_every_configured_device(obs_world):
    listed = devices(obs_world)
    for backend, name in obs_world.device_names.items():
        assert name in listed, backend
        entry = listed[name]
        assert entry["device"] and entry["name"], backend
        assert entry["api"] == {"major": 0, "minor": 4}, backend


def test_ptz_get_api_version_over_the_wire(obs_world):
    assert obs_world.ws.vendor(VENDOR, "ptz_get_api_version", {"device": device(obs_world, "visca-tcp")}) == {
        "major": 0, "minor": 4}


def test_move_reaches_the_camera(obs_world):
    uuid = device(obs_world, "visca-tcp")
    assert obs_world.ws.vendor(VENDOR, "ptz_move", {"device": uuid, "pan": 0.5, "tilt": -0.3}) == {}
    obs_world.wait_for_state(lambda s: abs(s["pan_speed"] - 0.5) < 0.05 and abs(s["tilt_speed"] + 0.3) < 0.05)
    assert obs_world.ws.vendor(VENDOR, "ptz_stop", {"device": uuid}) == {}
    obs_world.wait_for_state(lambda s: s["pan_speed"] == 0.0 and s["tilt_speed"] == 0.0)


def test_a_whole_number_is_taken_for_a_float(obs_world):
    """JSON can't say that 1 is a float, and a client's 1.0 may well arrive as 1"""
    uuid = device(obs_world, "visca-tcp")
    obs_world.ws.vendor(VENDOR, "ptz_move", {"device": uuid, "pan": 1, "tilt": 0})
    obs_world.wait_for_state(lambda s: s["pan_speed"] > 0.9)
    obs_world.ws.vendor(VENDOR, "ptz_stop", {"device": uuid})
    obs_world.wait_for_state(lambda s: s["pan_speed"] == 0.0)


def test_get_state_returns_an_object(obs_world):
    """A ptr the caller passes in to be filled comes back, filled, under its own name"""
    answer = obs_world.ws.vendor(VENDOR, "ptz_get_state", {"device": device(obs_world, "visca-tcp"), "state": {}})
    assert set(answer) == {"state"}
    assert answer["state"]["connected"] is True
    assert isinstance(answer["state"]["features"], dict)


def test_a_ptr_left_out_is_still_filled(obs_world):
    """The client has nothing to put in it: the binding makes the object the proc fills"""
    answer = obs_world.ws.vendor(VENDOR, "ptz_get_state", {"device": device(obs_world, "visca-tcp")})
    assert answer["state"]["connected"] is True


def test_a_returned_object_and_a_returned_string(obs_world):
    uuid = device(obs_world, "visca-tcp")
    listed = obs_world.ws.vendor(VENDOR, "ptz_preset_get_list", {"device": uuid})
    assert isinstance(listed["return"]["presets"], dict)
    report = obs_world.ws.vendor(VENDOR, "ptz_get_camera_report", {"device": uuid})
    assert isinstance(report["report"], str)


def test_get_api_info_over_the_wire(obs_world):
    info = obs_world.ws.vendor(VENDOR, "ptz_get_api_info", {"device": device(obs_world, "visca-tcp")})["return"]
    assert "ptz_move" in {p["name"] for p in info["procs"]}
    assert "ptz_state_changed" in {s["name"] for s in info["signals"]}


def test_unknown_device(obs_world):
    assert error_of(obs_world, "ptz_stop", {"device": "nope"}) == "unknown_device"
    assert error_of(obs_world, "ptz_stop", {}) == "unknown_device"


def test_a_source_that_is_no_device_is_an_unknown_device(obs_world):
    scene = obs_world.create_scene()
    uuid = next(s["sceneUuid"] for s in obs_world.ws.call("GetSceneList")["scenes"] if s["sceneName"] == scene)
    assert error_of(obs_world, "ptz_stop", {"device": uuid}) == "unknown_device"


def test_unknown_field(obs_world):
    uuid = device(obs_world, "visca-tcp")
    assert error_of(obs_world, "ptz_stop", {"device": uuid, "bogus": 1}) == "unknown_field"
    assert error_of(obs_world, "ptz_move", {"device": uuid, "pan": 0.1, "tlit": 0.0}) == "unknown_field"
    # and the camera was not moved by the request that was refused
    assert obs_world.state()["pan_speed"] == 0.0


def test_bad_type(obs_world):
    uuid = device(obs_world, "visca-tcp")
    assert error_of(obs_world, "ptz_preset_move", {"device": uuid, "id": "camera:1", "index": 1.5}) == "bad_type"
    assert error_of(obs_world, "ptz_preset_move", {"device": uuid, "id": 7, "index": 1}) == "bad_type"
    assert error_of(obs_world, "ptz_move", {"device": uuid, "pan": "fast"}) == "bad_type"
    assert error_of(obs_world, "ptz_get_state", {"device": uuid, "state": 3}) == "bad_type"
    assert error_of(obs_world, "ptz_preset_capture_thumbnail",
                    {"device": uuid, "id": "camera:1", "if_arrived": "yes"}) == "bad_type"


def test_list_discovery_has_an_instance_of_each_kind(obs_world):
    """Which a client can't make itself, so the binding does, and keeps them"""
    instances = obs_world.ws.vendor(VENDOR, "ptz_list_discovery")["instances"]
    assert instances
    assert all(i["kind"].endswith(".discovery") and i["device"] for i in instances)
    assert len({i["device"] for i in instances}) == len(instances) == len({i["kind"] for i in instances})
    assert obs_world.ws.vendor(VENDOR, "ptz_list_discovery")["instances"] == instances
    # and they are not devices
    listed = {d["device"] for d in obs_world.ws.vendor(VENDOR, "ptz_list_devices")["devices"]}
    assert not listed & {i["device"] for i in instances}


def test_a_discovery_instance_is_called_as_a_device_is(obs_world):
    instance = obs_world.ws.vendor(VENDOR, "ptz_list_discovery")["instances"][0]["device"]
    assert obs_world.ws.vendor(VENDOR, "ptz_get_api_version", {"device": instance}) == {"major": 0, "minor": 4}
    assert obs_world.ws.vendor(VENDOR, "ptz_discovery_start", {"device": instance}) == {}
    assert obs_world.ws.vendor(VENDOR, "ptz_discovery_stop", {"device": instance}) == {}


def test_a_proc_the_device_does_not_have(obs_world):
    """A discovery instance's proc is a request too, once there is an instance, which
    a device does not answer; nor an instance a device's"""
    instance = obs_world.ws.vendor(VENDOR, "ptz_list_discovery")["instances"][0]["device"]
    assert error_of(obs_world, "ptz_discovery_start", {"device": device(obs_world, "visca-tcp")}) == "no_such_proc"
    assert error_of(obs_world, "ptz_stop", {"device": instance}) == "no_such_proc"


def test_a_proc_that_cannot_cross_the_wire_is_no_request(obs_world):
    assert is_unknown_request(obs_world, "ptz_get_parent_source", {"device": device(obs_world, "visca-tcp")})


def current_access(obs_world):
    """What a client is let do, by what it finds it can"""
    if is_unknown_request(obs_world, "ptz_bridge_version"):
        return "off"
    if is_unknown_request(obs_world, "ptz_stop", {}):
        return "read-only"
    return "control"


@pytest.fixture
def access(obs_world):
    """Sets how much a client is let do, as the settings dialog does, and puts it
    back to the default"""
    def set_access(value):
        obs_world.run_ui_test("set_websocket_access", value=value)
        obs_world.wait_for(lambda: current_access(obs_world) == value)

    yield set_access
    set_access("control")


def test_access_is_control_unless_set(obs_world):
    assert current_access(obs_world) == "control"


def test_access_off_leaves_no_request(obs_world, access):
    uuid = device(obs_world, "visca-tcp")
    access("off")
    assert is_unknown_request(obs_world, "ptz_get_state", {"device": uuid})
    assert is_unknown_request(obs_world, "ptz_move", {"device": uuid, "pan": 0.5, "tilt": 0.0})
    assert is_unknown_request(obs_world, "ptz_list_devices")
    assert is_unknown_request(obs_world, "ptz_bridge_version")
    assert obs_world.state()["pan_speed"] == 0.0


def test_access_read_only_reads_and_does_not_control(obs_world, access):
    uuid = device(obs_world, "visca-tcp")
    access("read-only")
    assert obs_world.ws.vendor(VENDOR, "ptz_get_state", {"device": uuid})["state"]["connected"] is True
    assert device(obs_world, "visca-tcp") == uuid
    assert is_unknown_request(obs_world, "ptz_move", {"device": uuid, "pan": 0.5, "tilt": 0.0})
    assert is_unknown_request(obs_world, "ptz_preset_delete", {"device": uuid, "id": "camera:1"})
    assert is_unknown_request(obs_world, "ptz_get_parent_source", {"device": uuid})
    assert obs_world.state()["pan_speed"] == 0.0


def test_access_changes_at_once_for_a_client_that_stays(obs_world, access):
    """The same connection throughout: nothing is restarted, and no client dropped"""
    uuid = device(obs_world, "visca-tcp")
    client = obs_world.ws
    for value, stops in (("off", False), ("control", True), ("read-only", False), ("control", True)):
        access(value)
        assert obs_world.ws is client
        if stops:
            assert client.vendor(VENDOR, "ptz_stop", {"device": uuid}) == {}
        else:
            assert is_unknown_request(obs_world, "ptz_stop", {"device": uuid})


def test_access_is_kept_when_obs_is_restarted(obs_world, access):
    """In the plugin's own config, with its other settings"""
    access("read-only")
    obs_world.restart_obs()
    obs_world.wait_for(lambda: current_access(obs_world) == "read-only")
    assert json.loads((obs_world.config_dir / "config.json").read_text())["websocket_access"] == "read-only"
