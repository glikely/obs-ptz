"""Covers a device's transient *state* API (see test_device_settings.py for
the settings half): the "ptz_get_state" / "ptz_request_state" proc
handlers (PTZDevice::saveState() and requestState(), src/ptz-device.cpp).
The same "ptz_get_state" also backs PTZListModel's device-row cache -- one
proc for a device's whole state, whoever the caller is.

Driven through tests/ui-harness/device-state-test.cpp's "get_device_state"
and "set_device_state" tests (see World.device_state() in conftest.py).

VISCA-only where the state is: "wb_mode" and "focus_af_enabled" are what
a VISCA camera reports (see test_autofocus_white_balance.py, which drives
the older calldata spelling, "ptz_set"/"ptz_get", of the same state).
"""

import time

import pytest

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
