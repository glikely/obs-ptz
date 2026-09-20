"""End-to-end tests that the plugin itself -- not just ptzsim's own
/state -- notices a camera connecting/disconnecting.

These are driven through tests/ui-harness/device-status-test.cpp's
"get_device_status" test (see World.device_status()/
wait_for_device_status() in conftest.py), which is the only way to
observe PTZDevice::isConnected() from outside the plugin:
obs-websocket exposes no device list or property read of its own.

Connect/disconnect detection (PTZDevice::setConnected(), see
PTZVisca::cmd_get_camera_info()/timeout() in src/ptz-visca.cpp) is only
wired up for VISCA -- Pelco never calls setConnected() at all (obs-ptz
has no read-back for it), so isConnected() would just stay permanently
false there. These tests are VISCA-only for that reason.

Camera-initiated disconnect/reconnect needs its own, disposable ptzsim
instance (flaky_ptzsim, see conftest.py) rather than the suite's shared,
session-scoped one (ptzsim_process): killing that would break every
other test in this suite that still needs it.
"""

ACTION_PAN_TILT = 3


def test_connect_detected(obs_world, flaky_ptzsim, tmp_path):
    device_id = obs_world.device_ids["visca-tcp-flaky"]
    obs_world.wait_for_device_status(device_id, tmp_path / "status.json",
                                      lambda s: s["connected"] is True, timeout=10)


def test_disconnect_detected(obs_world, flaky_ptzsim, tmp_path):
    device_id = obs_world.device_ids["visca-tcp-flaky"]
    status_file = tmp_path / "status.json"
    obs_world.wait_for_device_status(device_id, status_file, lambda s: s["connected"] is True, timeout=10)

    flaky_ptzsim.stop()
    # An idle connection doesn't self-detect a drop -- PTZVisca::timeout()
    # is only ever armed by a command that's actually waiting on a reply
    # (see send_packet()), so something has to try to move the camera
    # before a timeout (and therefore setConnected(false)) can fire.
    obs_world.trigger_action(device_id, ACTION_PAN_TILT, pan_speed=0.3)

    obs_world.wait_for_device_status(device_id, status_file, lambda s: s["connected"] is False, timeout=10)


def test_reconnect_after_restart(obs_world, flaky_ptzsim, tmp_path):
    device_id = obs_world.device_ids["visca-tcp-flaky"]
    status_file = tmp_path / "status.json"
    obs_world.wait_for_device_status(device_id, status_file, lambda s: s["connected"] is True, timeout=10)

    flaky_ptzsim.stop()
    obs_world.trigger_action(device_id, ACTION_PAN_TILT, pan_speed=0.3)
    obs_world.wait_for_device_status(device_id, status_file, lambda s: s["connected"] is False, timeout=10)

    flaky_ptzsim.start()
    obs_world.wait_for_device_status(device_id, status_file, lambda s: s["connected"] is True, timeout=10)
