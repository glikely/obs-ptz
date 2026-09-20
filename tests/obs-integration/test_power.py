"""End-to-end tests for camera power on/off (PTZDevice::set()'s
"power_on" handling in PTZVisca::set(), src/ptz-visca.cpp).

PTZ_ACTION_POWER_OFF/PTZ_ACTION_POWER_ON exist in ptz_action_source's own
PTZ_ACTION_* enum (src/ptz-action-source.c) but are never actually wired
up in ptz_action_source_do_action() (they fall through to its
"default: break;") or exposed in its own "action" property list, so
power is no more reachable over obs-websocket than autofocus/white
balance are (see test_autofocus_white_balance.py's own docstring).
Driven the same way: set_device/get_device_status
(tests/ui-harness/set-device-test.cpp/device-status-test.cpp).

VISCA-only: Pelco never overrides PTZDevice::set() at all, so a
power_on request there is a silent no-op (the base PTZDevice::set()
doesn't handle "power_on" either -- only VISCA does).

Like "wb_mode" and unlike "focus_af_enabled", PTZVisca::set() doesn't
locally cache "power_on" -- VISCA_CAM_Power's own "affects" field marks
it stale so the plugin re-queries CAM_PowerInq once the set command
completes, so this is a genuine "picked up what the camera reported
back" check, not an echo of what was sent.
"""

import pytest

VISCA_BACKENDS = ["visca-tcp", "visca-udp", "visca-serial"]


@pytest.mark.parametrize("backend", VISCA_BACKENDS)
def test_power_toggle(obs_world, backend, tmp_path):
    device_id = obs_world.device_ids[backend]
    status_file = tmp_path / "status.json"

    obs_world.run_ui_test("set_device", device_id=device_id, power_on=False)
    obs_world.wait_for_device_status(device_id, status_file, lambda s: s["power_on"] is False, timeout=5)

    obs_world.run_ui_test("set_device", device_id=device_id, power_on=True)
    obs_world.wait_for_device_status(device_id, status_file, lambda s: s["power_on"] is True, timeout=5)
