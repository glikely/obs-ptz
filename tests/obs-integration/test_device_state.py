"""Covers a device's transient *state* API (see test_device_settings.py for
the settings half): the "ptz_get_state" / "ptz_request_state" proc
handlers (PTZDevice::saveState() and requestState(), src/ptz-device.cpp).
The same "ptz_get_state" also backs PTZListModel's device-row cache -- one
proc for a device's whole state, whoever the caller is.

Driven through tests/ui-harness/device-state-test.cpp's "get_device_state"
and "set_device_state" tests (see World.device_state() in conftest.py).

VISCA-only where the state is: "wb_mode" and "focus_af_enabled" are what
a VISCA camera reports (see test_autofocus_white_balance.py).
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


def settled_sent_count(obs_world, device_id, out):
    """How many commands (not inquiries, which the device keeps sending to
    see where the camera is) the device has sent, once it has stopped
    sending them"""
    before = visca_sent_count(obs_world, device_id, out)
    for _ in range(15):
        time.sleep(0.7)
        now = visca_sent_count(obs_world, device_id, out)
        if now == before:
            return now
        before = now
    raise AssertionError("the device never stopped sending")


def visca_sent_count(obs_world, device_id, out):
    return obs_world.device_state(device_id, out)["state"]["statistics"].get("visca_command_count", 0)


def test_a_request_asks_for_just_what_it_holds(obs_world, tmp_path):
    device_id = obs_world.device_ids["visca-tcp"]
    out = tmp_path / "state.json"

    obs_world.wait_for_device_state(device_id, out, lambda r: r["state"].get("connected") is True, timeout=10)
    obs_world.run_ui_test("set_device_state", device_id=device_id, wb_mode=1)
    obs_world.wait_for_device_state(device_id, out, lambda r: r["state"]["wb_mode"] == 1, timeout=5)
    before = settled_sent_count(obs_world, device_id, out)

    # an empty request asks for nothing
    obs_world.run_ui_test("set_device_state", device_id=device_id)
    time.sleep(1.0)
    assert visca_sent_count(obs_world, device_id, out) == before

    # a value is asked for even if it is what the camera already reports,
    # which may not be what it is doing any more
    obs_world.run_ui_test("set_device_state", device_id=device_id, wb_mode=1)
    obs_world.wait_for(lambda: visca_sent_count(obs_world, device_id, out) > before, timeout=5)


@pytest.mark.parametrize("name", ["focus_onetouch", "wb_onepush"])
def test_a_trigger_sends_its_command(obs_world, name, tmp_path):
    device_id = obs_world.device_ids["visca-tcp"]
    out = tmp_path / "state.json"
    obs_world.wait_for_device_state(device_id, out, lambda r: r["state"].get("connected") is True, timeout=10)
    before = settled_sent_count(obs_world, device_id, out)

    obs_world.run_ui_test("trigger_device", device_id=device_id, name=name)
    obs_world.wait_for(lambda: visca_sent_count(obs_world, device_id, out) > before, timeout=5)


def test_an_unknown_trigger_does_nothing(obs_world, tmp_path):
    device_id = obs_world.device_ids["visca-tcp"]
    out = tmp_path / "state.json"
    obs_world.wait_for_device_state(device_id, out, lambda r: r["state"].get("connected") is True, timeout=10)
    before = settled_sent_count(obs_world, device_id, out)

    obs_world.run_ui_test("trigger_device", device_id=device_id, name="no_such_trigger")
    time.sleep(1.0)
    assert visca_sent_count(obs_world, device_id, out) == before


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


def test_discovering_the_movement_limits(obs_world, cameras, tmp_path):  # noqa: F811
    """On a camera of its own: it drives the camera from end to end, and
    back to where it was, which takes a while."""
    cameras.add_source(obs_world.create_scene(), "limits-cam")
    cameras.add_filter("limits-cam")
    out = tmp_path / "device.json"
    device_id = obs_world.wait_for_device_by_name("limits-cam", out, lambda r: r["found"] and r["bound"])["device_id"]
    out = tmp_path / "state.json"
    obs_world.wait_for_device_state(device_id, out, lambda r: r["state"].get("connected") is True, timeout=10)

    def limits():
        saved = obs_world.device_settings(device_id, tmp_path / "settings.json")["saved"]
        return {k: saved[k] for k in saved if k.startswith("visca_") and k.endswith(("_range", "_far", "_near"))}

    assert limits() == {"visca_pan_range": 0x1400, "visca_tilt_range": 0x500, "visca_zoom_range": 0x7ac0,
                        "visca_focus_far": 0x1000, "visca_focus_near": 0xf000}
    assert obs_world.device_state(device_id, out)["state"]["focus_af_enabled"] is True
    obs_world.run_ui_test("trigger_device", device_id=device_id, name="discover_limits")
    obs_world.wait_for(lambda: limits()["visca_zoom_range"] != 0x7ac0, timeout=150, interval=2)
    found = limits()
    # the ends of the sim's travel (scripts/ptzsim/backends/visca.py), focus
    # too, though the camera started out focusing by itself
    assert found == {"visca_pan_range": 0x2800, "visca_tilt_range": 0x2800, "visca_zoom_range": 0xe500,
                     "visca_focus_far": 0, "visca_focus_near": 0xe500}
    # and focusing by itself again
    obs_world.wait_for_device_state(device_id, out, lambda r: r["state"]["focus_af_enabled"] is True, timeout=10)


@pytest.mark.parametrize("backend", ["visca-tcp", "visca-udp", "visca-serial"])
def test_the_camera_is_polled_as_fast_as_it_answers(obs_world, backend, tmp_path):
    """The poll waits for the camera to answer the last one, then a period (200ms)
    before the next, so it can't come faster than that, and the statistics say
    how fast it does."""
    out = tmp_path / "state.json"
    stats = obs_world.wait_for_device_state(
        obs_world.device_ids[backend], out,
        lambda r: r["state"]["statistics"].get("visca_poll_count", 0) >= 5
        and "visca_polls_per_second" in r["state"]["statistics"], timeout=15)["state"]["statistics"]

    # (the period is 200ms, and a timer may fire a millisecond or two early)
    assert 0 < stats["visca_polls_per_second"] <= 1000 / 190
    assert stats["visca_poll_cycle_ms"] >= 0


@pytest.mark.parametrize("backend", ["visca-tcp", "visca-udp", "visca-serial"])
def test_the_traffic_to_and_from_the_camera_is_measured(obs_world, backend, tmp_path):
    """Polling alone keeps packets going both ways, a second's worth of which
    the statistics report as rates, and as counts of bytes"""
    rates = ["visca_sent_packets_per_second", "visca_recv_packets_per_second",
             "visca_sent_bytes_per_second", "visca_recv_bytes_per_second"]
    stats = obs_world.wait_for_device_state(
        obs_world.device_ids[backend], tmp_path / "state.json",
        lambda r: all(r["state"]["statistics"].get(k, 0) > 0 for k in rates), timeout=15)["state"]["statistics"]

    # an inquiry is five bytes at the least, and a reply is more
    assert stats["visca_sent_bytes"] >= 5 * stats["visca_sent_count"]
    assert stats["visca_recv_bytes"] >= 3 * stats["visca_recv_count"]
    assert stats["visca_recv_bytes_per_second"] > stats["visca_sent_bytes_per_second"] * 0.5
