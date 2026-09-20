"""End-to-end tests that the plugin itself -- not just ptzsim's own
/state -- notices a camera connecting/disconnecting and picks up
position updates the camera reports back.

These are driven through tests/ui-harness/device-status-test.cpp's
"get_device_status" test (see World.device_status()/
wait_for_device_status() in conftest.py), which is the only way to
observe PTZDevice::isConnected() and its cached pan/tilt position from
outside the plugin: obs-websocket exposes no device list or property
read of its own.

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
ACTION_STOP = 4


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


def test_receives_position_updates_while_moving(obs_world, tmp_path):
    """A pan/tilt drive command and the stop that follows it both mark
    "pan_pos" stale (see VISCA_PanTilt_drive's "affects" field in
    src/ptz-visca.cpp), so the plugin re-queries Pan-tiltPosInq once the
    stop completes. Its cached pan_pos afterwards should reflect the
    camera's real, updated position -- not just whatever value was
    cached at startup or left over from an earlier test."""
    device_id = obs_world.device_ids["visca-tcp"]
    status_file = tmp_path / "status.json"

    before = obs_world.device_status(device_id, status_file)

    obs_world.trigger_action(device_id, ACTION_PAN_TILT, pan_speed=0.6, tilt_speed=0.0)
    obs_world.wait_for_state(lambda s: abs(s["pan"]) > 0.05, timeout=5)
    obs_world.trigger_action(device_id, ACTION_STOP)
    real_pan = obs_world.wait_for_state(lambda s: s["pan_speed"] == 0.0, timeout=5)["pan"]

    after = obs_world.wait_for_device_status(
        device_id, status_file, lambda s: s["pan_pos"] != before["pan_pos"], timeout=5)

    # Same sign as ptzsim's own real position -- confirms the plugin
    # decoded the camera's actual reported direction of travel, not just
    # that *some* value changed.
    assert (after["pan_pos"] > 0) == (real_pan > 0)
