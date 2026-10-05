"""Covers ONVIF reporting where the camera is (PTZOnvif::handleGetStatusResponse()
in src/ptz-onvif.cpp), as the generic position part of the device state:
"pan" and "tilt" in [-1, 1], "zoom" in [0, 1], and no "focus", which ONVIF's
status has no position for.

Talks ONVIF to the shared ptzsim, which is the same camera the VISCA and Pelco
devices drive, so a move made through any of them shows up in ptzsim's own
debug state, which is what the device's reported position is compared with.
"""

import time

import pytest

ACTION_PAN_TILT = 3
ACTION_STOP = 4


@pytest.fixture
def onvif_device(obs_world, tmp_path):
    device_name = obs_world.device_names["onvif"]
    obs_world.wait_for_device_state(
        device_name, tmp_path / "state.json", lambda r: r["state"].get("connected") is True, timeout=20)
    return device_name


def test_state_has_the_axes_onvif_reports(obs_world, onvif_device, tmp_path):
    state = obs_world.wait_for_device_state(
        onvif_device, tmp_path / "state.json", lambda r: {"pan", "tilt", "zoom"} <= set(r["state"]), timeout=15)["state"]
    assert "focus" not in state


def test_reports_the_position_it_reads_back(obs_world, onvif_device, tmp_path):
    out = tmp_path / "state.json"
    state = obs_world.wait_for_device_state(
        onvif_device, out, lambda r: {"pan", "tilt", "zoom"} <= set(r["state"]), timeout=15)["state"]
    assert -1.0 <= state["pan"] <= 1.0
    assert -1.0 <= state["tilt"] <= 1.0
    assert 0.0 <= state["zoom"] <= 1.0
    assert "focus" not in state


def test_position_follows_a_move_as_it_happens(obs_world, onvif_device, tmp_path):
    out = tmp_path / "state.json"
    start = obs_world.wait_for_device_state(
        onvif_device, out, lambda r: "pan" in r["state"], timeout=15)["state"]["pan"]

    # towards the middle from either end, so there is room to move
    obs_world.trigger_action(onvif_device, ACTION_PAN_TILT, pan_speed=-0.3 if start > 0 else 0.3, tilt_speed=0.0)
    seen = set()
    for _ in range(12):
        time.sleep(0.25)
        seen.add(round(obs_world.device_state(onvif_device, out)["state"]["pan"], 3))
    obs_world.trigger_action(onvif_device, ACTION_STOP)

    # The status is only read every five seconds at rest, so seeing the
    # position pass through several values in three seconds means it is
    # read much more often than that while the camera moves
    assert len(seen) >= 3, f"position only read as {sorted(seen)}"

    # and once stopped it settles on where the camera actually is
    camera = obs_world.wait_for_state(lambda s: s["pan_speed"] == 0.0)
    obs_world.wait_for_device_state(
        onvif_device, out, lambda r: abs(r["state"]["pan"] - camera["pan"]) < 0.02, timeout=3)
