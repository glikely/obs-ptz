"""VISCA power on when OBS starts and off when it closes: the
"power_on_at_startup" and "power_off_at_shutdown" settings, and
PTZVisca::onOBSStartup()/onOBSShutdown() in src/ptz-visca.cpp.

PTZDevice hears of OBS having finished loading, and of it closing just
before it clears its scenes, from OBS itself (PTZDevice::onFrontendEvent()),
not through the UI. The first is seen here as it happens, as OBS starts, by a
camera that is off until then (power_ptzsim). Closing OBS ends the session, so
the second is looked at when it has (obs_world's teardown asserts the cameras
are off), and the tests hand either event to a device by hand with the
"obs_event" test (tests/ui-harness/device-state-test.cpp), since OBS does not
send them twice.
"""

import time


def test_camera_is_powered_on_when_obs_starts(obs_world, power_ptzsim):
    obs_world.wait_for(lambda: power_ptzsim.power() is True, timeout=30)


def test_camera_that_is_not_there_yet_is_powered_on_when_it_answers(obs_world, late_power_ptzsim):
    device_id = obs_world.device_ids["visca-tcp-power-late"]

    # OBS has started, and the camera is not there to be sent a command
    obs_world.run_ui_test("obs_event", device_id=device_id, event="startup")
    time.sleep(1)

    # It is, some time later, and the device connects to it
    late_power_ptzsim.start()
    assert late_power_ptzsim.power() is False
    obs_world.wait_for(lambda: late_power_ptzsim.power() is True, timeout=20)


def test_camera_is_powered_off_when_obs_closes(obs_world, power_ptzsim):
    device_id = obs_world.device_ids["visca-tcp-power"]
    obs_world.wait_for(lambda: power_ptzsim.power() is True, timeout=30)

    obs_world.run_ui_test("obs_event", device_id=device_id, event="shutdown")
    obs_world.wait_for(lambda: power_ptzsim.power() is False, timeout=10)

    # Leave it on, as OBS would have, for whether closing turns it off
    obs_world.run_ui_test("set_device_state", device_id=device_id, power_on=True)
    obs_world.wait_for(lambda: power_ptzsim.power() is True, timeout=10)


def test_a_camera_is_left_alone_unless_it_is_set_to_be_powered(obs_world):
    # The shared ptzsim's VISCA-over-TCP device has neither setting
    device_id = obs_world.device_ids["visca-tcp"]
    obs_world.run_ui_test("set_device_state", device_id=device_id, power_on=True)
    obs_world.wait_for_state(lambda s: s["power"] is True, timeout=5)

    obs_world.run_ui_test("obs_event", device_id=device_id, event="shutdown")
    time.sleep(1.5)
    assert obs_world.state()["power"] is True

    obs_world.run_ui_test("obs_event", device_id=device_id, event="startup")
    time.sleep(1.5)
    assert obs_world.state()["power"] is True


def test_camera_of_a_filter_is_powered_off_when_obs_closes(obs_world, filter_power_ptzsim, tmp_path):
    """Left running when the test ends: closing OBS ends the session, and
    obs_world's teardown asserts the camera is off by then. A filter's device
    is gone once OBS has cleared its scenes, which it does before it says it
    is exiting, so it has to be turned off before that."""
    # Not a scene from create_scene(), which is removed at the end of the
    # test, and the source and its filter with it
    scene = "power-filter-scene"
    obs_world.ws.call("CreateScene", {"sceneName": scene})
    obs_world.ws.call("CreateInput", {
        "sceneName": scene,
        "inputName": "power-filter-cam",
        "inputKind": "ffmpeg_source",
        "inputSettings": {"is_local_file": True, "local_file": ""},
    })
    obs_world.ws.call("CreateSourceFilter", {
        "sourceName": "power-filter-cam",
        "filterName": "PTZ",
        "filterKind": "ca.secretlab.obs-ptz.visca",
        "filterSettings": {
            "type": "visca-over-tcp",
            "host": "127.0.0.1",
            "tcp_port": filter_power_ptzsim.tcp_port,
            "power_off_at_shutdown": True,
        },
    })
    device_id = obs_world.wait_for_device_by_name(
        "power-filter-cam", tmp_path / "device.json", lambda r: r["found"] and r["bound"])["device_id"]
    obs_world.wait_for_device_status(device_id, tmp_path / "status.json", lambda s: s["connected"], timeout=10)

    obs_world.run_ui_test("set_device_state", device_id=device_id, power_on=True)
    obs_world.wait_for(lambda: filter_power_ptzsim.power() is True, timeout=10)
