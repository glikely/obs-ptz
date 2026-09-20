"""End-to-end tests for autofocus mode (PTZDevice::set_autofocus(),
"focus_af_enabled") and white balance mode ("wb_mode") -- both settable
and readable via the "ptz_set"/"ptz_get" proc handlers
(src/ptz-device.cpp/src/ptz-visca.cpp), the same ones
PTZControls::on_deviceList_customContextMenuRequested()/
getCurrentDeviceAutofocus() already use from inside the UI
(src/ptz-controls.cpp).

Neither has a ptz_action_source action type of its own -- its own
PTZ_ACTION_* enum (src/ptz-action-source.c) only covers continuous
pan/tilt, stop, presets and power -- so both are driven through
tests/ui-harness/set-device-test.cpp's "set_device" test and read back
through device-status-test.cpp's "get_device_status" test (see
World.device_status()/wait_for_device_status() in conftest.py), the
same way as test_device_status.py.

VISCA-only: "wb_mode" is a VISCA-specific property (PTZDevice::get()
doesn't know it, only PTZVisca::get() does), and Pelco has no
autofocus concept in this plugin either (PTZDevice::set_autofocus() is
an empty no-op unless a backend overrides it, see src/ptz-device.hpp).

set_autofocus() updates its cached "focus_af_enabled" locally as soon
as the command is sent (see PTZVisca::set_autofocus() in
src/ptz-visca.cpp), so that test mostly confirms the round trip through
the real device object and a real, acked wire command. "wb_mode" has no
such local update (see PTZVisca::set()) -- it only becomes correct once
the follow-up CameraControlInq that VISCA_CAM_WB_Mode's own "affects"
field triggers actually completes, so that test is a genuine "picked up
what the camera reported back" check.
"""

import pytest

VISCA_BACKENDS = ["visca-tcp", "visca-udp", "visca-serial"]


@pytest.mark.parametrize("backend", VISCA_BACKENDS)
def test_autofocus_toggle(obs_world, backend, tmp_path):
    device_id = obs_world.device_ids[backend]
    status_file = tmp_path / "status.json"

    obs_world.run_ui_test("set_device", device_id=device_id, focus_af_enabled=False)
    obs_world.wait_for_device_status(
        device_id, status_file, lambda s: s["focus_af_enabled"] is False, timeout=5)

    obs_world.run_ui_test("set_device", device_id=device_id, focus_af_enabled=True)
    obs_world.wait_for_device_status(
        device_id, status_file, lambda s: s["focus_af_enabled"] is True, timeout=5)


@pytest.mark.parametrize("backend", VISCA_BACKENDS)
@pytest.mark.parametrize("mode", [0, 1, 2, 5])
def test_white_balance_mode(obs_world, backend, mode, tmp_path):
    device_id = obs_world.device_ids[backend]
    status_file = tmp_path / "status.json"

    obs_world.run_ui_test("set_device", device_id=device_id, wb_mode=mode)
    obs_world.wait_for_device_status(device_id, status_file, lambda s: s["wb_mode"] == mode, timeout=5)
