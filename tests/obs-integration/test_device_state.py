"""Covers a device's transient *state* API (see test_device_settings.py for
the settings half): the "ptz_get_state" / "ptz_request_state" proc
handlers (PTZDevice::saveState() and requestState(), src/ptz-device.cpp).
The same "ptz_get_state" also backs PTZListModel's device-row cache -- one
proc for a device's whole state, whoever the caller is.

Driven through tests/ui-harness/device-state-test.cpp's "get_device_state"
and "set_device_state" tests (see World.device_state() in conftest.py).

VISCA-only where the state is: "wb_mode" and "focus_af_enabled" are what
a VISCA camera reports (see test_autofocus_white_balance.py, which drives
the older calldata spelling, "ptz_set", of the same requests).
"""

import time

import pytest

# The filter fixtures live in test_filter_devices.py
from test_filter_devices import camera_sim, cameras  # noqa: F401

VISCA_BACKENDS = ["visca-tcp", "visca-udp", "visca-serial"]
BASE_STATE_KEYS = {"name", "connected", "live", "preview", "locked"}


@pytest.mark.parametrize("backend", VISCA_BACKENDS)
def test_state_reports_the_device(obs_world, backend, tmp_path):
    device_id = obs_world.device_ids[backend]
    result = obs_world.wait_for_device_state(
        device_id, tmp_path / "state.json", lambda r: r["state"].get("connected") is True and "wb_mode" in r["state"],
        timeout=10)

    state = result["state"]
    assert BASE_STATE_KEYS <= set(state)
    assert {"power_on", "focus_af_enabled", "statistics"} <= set(state)


@pytest.mark.parametrize("backend", VISCA_BACKENDS)
@pytest.mark.parametrize("mode", [0, 1, 2, 5])
def test_requested_white_balance_mode_is_picked_up(obs_world, backend, mode, tmp_path):
    device_id = obs_world.device_ids[backend]
    out = tmp_path / "state.json"
    # not the mode asked for, so this is a change whichever was left before
    other = 1 if mode != 1 else 2
    obs_world.run_ui_test("set_device_state", device_id=device_id, wb_mode=other)
    obs_world.wait_for_device_state(device_id, out, lambda r: r["state"]["wb_mode"] == other, timeout=5)

    obs_world.run_ui_test("set_device_state", device_id=device_id, wb_mode=mode)
    obs_world.wait_for_device_state(device_id, out, lambda r: r["state"]["wb_mode"] == mode, timeout=5)


def test_a_request_asks_for_just_what_it_holds(obs_world, tmp_path):
    device_id = obs_world.device_ids["visca-tcp"]
    out = tmp_path / "state.json"

    def sent():
        return obs_world.device_state(device_id, out)["state"]["statistics"].get("visca_sent_count", 0)

    obs_world.wait_for_device_state(device_id, out, lambda r: r["state"].get("connected") is True, timeout=10)
    obs_world.run_ui_test("set_device_state", device_id=device_id, wb_mode=1)
    obs_world.wait_for_device_state(device_id, out, lambda r: r["state"]["wb_mode"] == 1, timeout=5)

    # let the follow-up inquiries finish, so nothing else is being sent
    before = sent()
    for _ in range(15):
        time.sleep(0.7)
        now = sent()
        if now == before:
            break
        before = now
    else:
        raise AssertionError("the device never stopped sending")

    # an empty request asks for nothing
    obs_world.run_ui_test("set_device_state", device_id=device_id)
    time.sleep(1.0)
    assert sent() == before

    # a value is asked for even if it is what the camera already reports,
    # which may not be what it is doing any more
    obs_world.run_ui_test("set_device_state", device_id=device_id, wb_mode=1)
    obs_world.wait_for(lambda: sent() > before, timeout=5)


@pytest.mark.parametrize("backend", VISCA_BACKENDS)
def test_position_is_reported_in_the_units_of_the_movement_api(obs_world, backend, tmp_path):
    """The raw positions VISCA reads back, over the ranges its absolute moves
    use (src/ptz-visca.cpp), clamped: pan and tilt to [-1, 1], zoom and focus
    to [0, 1]."""
    state = obs_world.wait_for_device_state(
        obs_world.device_ids[backend], tmp_path / "state.json",
        lambda r: {"pan", "tilt", "zoom", "focus"} <= set(r["state"]), timeout=10)["state"]

    def clamp(value, low):
        return max(low, min(1.0, value))

    assert state["pan"] == pytest.approx(clamp(state["pan_pos"] / 0x1400, -1.0), abs=1e-3)
    assert state["tilt"] == pytest.approx(clamp(state["tilt_pos"] / 0x500, -1.0), abs=1e-3)
    assert state["zoom"] == pytest.approx(clamp(state["zoom_pos"] / 0x7ac0, 0.0), abs=1e-3)
    assert state["focus"] == pytest.approx(clamp((state["focus_pos"] - 0x1000) / (0xf000 - 0x1000), 0.0), abs=1e-3)


@pytest.mark.parametrize("backend", ["pelco-d", "pelco-p"])
def test_pelco_state_has_no_white_balance_or_position(obs_world, backend, tmp_path):
    state = obs_world.device_state(obs_world.device_ids[backend], tmp_path / "state.json")["state"]
    assert BASE_STATE_KEYS <= set(state)
    assert not {"wb_mode", "pan", "tilt", "zoom", "focus"} & set(state)


# What describes the device rather than what it reports, which ptz_get_state
# adds for PTZListModel's row: "type" is a setting too, the rest are neither.
DESCRIPTION_KEYS = {"name", "type", "description", "supports_set_home", "supports_diagnostics"}


def test_settings_properties_hold_no_camera_state(obs_world, cameras, tmp_path):  # noqa: F811
    """A filter's settings properties are what OBS's Filters dialog edits and
    saves, so nothing the camera reports may be among them."""
    cameras.add_source(obs_world.create_scene(), "split-cam")
    cameras.add_filter("split-cam")
    out = tmp_path / "device.json"
    device_id = obs_world.wait_for_device_by_name("split-cam", out, lambda r: r["found"] and r["bound"])["device_id"]

    state = obs_world.wait_for_device_state(
        device_id, tmp_path / "state.json", lambda r: "wb_mode" in r["state"], timeout=10)["state"]
    settings = obs_world.device_settings(device_id, tmp_path / "settings.json")["property_keys"]
    assert settings & (set(state) - DESCRIPTION_KEYS) == set()


@pytest.mark.parametrize("backend,supported", [("visca-tcp", True), ("pelco-d", False)])
def test_diagnostics_are_advertised(obs_world, backend, supported, tmp_path):
    state = obs_world.device_state(obs_world.device_ids[backend], tmp_path / "state.json")["state"]
    assert state["supports_diagnostics"] is supported


def test_scanning_inquiries_asks_the_camera_everything(obs_world, cameras, tmp_path):  # noqa: F811
    """On a camera of its own: the scan queues hundreds of inquiries, which
    keeps the device busy for a while."""
    cameras.add_source(obs_world.create_scene(), "scan-cam")
    cameras.add_filter("scan-cam")
    out = tmp_path / "device.json"
    device_id = obs_world.wait_for_device_by_name("scan-cam", out, lambda r: r["found"] and r["bound"])["device_id"]
    out = tmp_path / "state.json"

    def sent():
        return obs_world.device_state(device_id, out)["state"]["statistics"].get("visca_sent_count", 0)

    obs_world.wait_for_device_state(device_id, out, lambda r: r["state"].get("connected") is True, timeout=10)
    before = sent()
    obs_world.run_ui_test("set_device", device_id=device_id, trigger="scan_inquiries_trigger")
    # one inquiry for each of the 0x7e camera inquiry numbers, and more, on
    # top of the device's own polling
    obs_world.wait_for(lambda: sent() >= before + 0x7e, timeout=20)
