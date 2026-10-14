"""End-to-end tests for camera power on/off (the "power_on" request
handled by PTZVisca::requestState(), src/drivers/visca/ptz-visca.cpp).

PTZ_ACTION_POWER_OFF/PTZ_ACTION_POWER_ON exist in ptz_action_source's own
PTZ_ACTION_* enum (src/controller/ptz-action-source.c) but are never actually wired
up in ptz_action_source_do_action() (they fall through to its
"default: break;") or exposed in its own "action" property list, so
power is no more reachable over obs-websocket than autofocus/white
balance are (see test_autofocus_white_balance.py's own docstring).
Driven the same way: World.set_device_state()/device_status(), the
"ptz_request_state"/"ptz_get_state" procs over obs-websocket.

VISCA-only: Pelco never overrides PTZDevice::requestState() at all, so
a power_on request there is a silent no-op (the base requestState()
doesn't handle "power_on" either -- only VISCA does).

Like "wb_mode" and unlike "focus_af_enabled", PTZVisca doesn't
locally cache "power_on" -- VISCA_CAM_Power's own "affects" field marks
it stale so the plugin re-queries CAM_PowerInq once the set command
completes, so this is a genuine "picked up what the camera reported
back" check, not an echo of what was sent.
"""

import pytest

VISCA_BACKENDS = ["visca-tcp", "visca-udp", "visca-serial"]


@pytest.mark.parametrize("backend", VISCA_BACKENDS)
def test_power_toggle(obs_world, backend, tmp_path):
    device_name = obs_world.device_names[backend]
    status_file = tmp_path / "status.json"

    obs_world.set_device_state(device_name, power_on=False)
    obs_world.wait_for_device_status(device_name, status_file, lambda s: s["power_on"] is False, timeout=5)

    obs_world.set_device_state(device_name, power_on=True)
    obs_world.wait_for_device_status(device_name, status_file, lambda s: s["power_on"] is True, timeout=5)
