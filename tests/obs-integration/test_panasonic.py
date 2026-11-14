"""End-to-end tests for the Panasonic HTTP CGI driver (PTZPanasonic).

Exercises the real OBS plugin talking HTTP CGI to ptzsim's Panasonic backend:
- /live/camdata.html status polling (pan, tilt, zoom, focus, autofocus, power)
- Continuous pan/tilt/zoom/focus drive + rapid-update stop guarantee
- Absolute moves, home, autofocus toggle, and power state
"""

import time

import pytest

ACTION_PAN_TILT = 3
ACTION_STOP = 4


@pytest.fixture
def panasonic_device(obs_world, tmp_path):
    device_name = obs_world.device_names["panasonic"]
    obs_world.wait_for_device_state(
        device_name, tmp_path / "state.json", lambda r: r["state"].get("connected") is True, timeout=15)
    return device_name


def test_camdata_reports_all_axes_and_flags(obs_world, panasonic_device, tmp_path):
    state = obs_world.wait_for_device_state(
        panasonic_device,
        tmp_path / "state.json",
        lambda r: {"pan", "tilt", "zoom", "focus", "focus_af_enabled", "power_on"} <= set(r["state"]),
        timeout=10,
    )["state"]
    assert -1.0 <= state["pan"] <= 1.0
    assert -1.0 <= state["tilt"] <= 1.0
    assert 0.0 <= state["zoom"] <= 1.0
    assert 0.0 <= state["focus"] <= 1.0


def test_rapid_velocity_updates_always_deliver_stop(obs_world, panasonic_device):
    for i in range(10):
        speed = 0.2 + 0.05 * i
        obs_world.trigger_action(panasonic_device, ACTION_PAN_TILT, pan_speed=speed, tilt_speed=-speed / 2)
        time.sleep(0.01)

    obs_world.trigger_action(panasonic_device, ACTION_STOP)
    obs_world.wait_for_state(lambda s: s["pan_speed"] == 0.0 and s["tilt_speed"] == 0.0, timeout=5)


def test_autofocus_and_power_roundtrip(obs_world, panasonic_device, tmp_path):
    status_file = tmp_path / "status.json"

    obs_world.set_device_state(panasonic_device, focus_af_enabled=False)
    obs_world.wait_for_device_status(
        panasonic_device, status_file, lambda s: s.get("focus_af_enabled") is False, timeout=5)

    obs_world.set_device_state(panasonic_device, focus_af_enabled=True)
    obs_world.wait_for_device_status(
        panasonic_device, status_file, lambda s: s.get("focus_af_enabled") is True, timeout=5)

    obs_world.set_device_state(panasonic_device, power_on=False)
    obs_world.wait_for_state(lambda s: s["power"] is False, timeout=5)
    obs_world.wait_for_device_status(
        panasonic_device, status_file, lambda s: s.get("power_on") is False, timeout=5)

    obs_world.set_device_state(panasonic_device, power_on=True)
    obs_world.wait_for_state(lambda s: s["power"] is True, timeout=5)
    obs_world.wait_for_device_status(
        panasonic_device, status_file, lambda s: s.get("power_on") is True, timeout=5)


def test_position_polls_updated_camera_state(obs_world, panasonic_device, tmp_path):
    out = tmp_path / "state.json"
    start = obs_world.wait_for_device_state(
        panasonic_device, out, lambda r: "pan" in r["state"], timeout=10)["state"]["pan"]

    obs_world.trigger_action(panasonic_device, ACTION_PAN_TILT, pan_speed=-0.4 if start > 0 else 0.4, tilt_speed=0.0)
    time.sleep(0.8)
    obs_world.trigger_action(panasonic_device, ACTION_STOP)

    camera = obs_world.wait_for_state(lambda s: s["pan_speed"] == 0.0, timeout=5)
    obs_world.wait_for_device_state(
        panasonic_device, out, lambda r: abs(r["state"]["pan"] - camera["pan"]) < 0.02, timeout=5)

