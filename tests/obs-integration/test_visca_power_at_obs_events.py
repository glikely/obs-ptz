"""VISCA power on when OBS starts and off when it closes: the
"power_on_at_startup" and "power_off_at_shutdown" settings, and
PTZVisca::onOBSStartup()/onOBSShutdown() in src/ptz-visca.cpp.

A PTZDevice hears of OBS having finished loading, and of it closing just
before it clears its scenes, from OBS itself (PTZDevice::onFrontendEvent()),
so these tests need OBS to start and to close, which it does once each. The
session starts it with cameras that are off, and the first tests look at what
has been done to them since. Closing it ends the session, so what it does
then is looked at when it has: obs_world's teardown asserts the cameras that
should be off are, and the one that should be left alone is.
"""


def test_camera_is_powered_on_when_obs_starts(obs_world, power_ptzsim):
    obs_world.wait_for(lambda: power_ptzsim.power() is True, timeout=30)


def test_camera_that_was_not_there_when_obs_started_is_powered_on_when_it_answers(obs_world, late_power_ptzsim):
    # obs_world starts it just after OBS has finished loading, when the
    # device was told to power it on and it was not there to be sent it
    obs_world.wait_for(lambda: late_power_ptzsim.power() is True, timeout=30)


def test_camera_is_powered_off_when_obs_closes(obs_world, power_ptzsim):
    """Left on for OBS to turn off: obs_world's teardown asserts it is."""
    obs_world.wait_for(lambda: power_ptzsim.power() is True, timeout=30)


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
    device_name = obs_world.wait_for_device_by_name(
        "power-filter-cam", tmp_path / "device.json", lambda r: r["found"] and r["bound"])["uuid"]
    obs_world.wait_for_device_status(device_name, tmp_path / "status.json", lambda s: s["connected"], timeout=10)

    obs_world.run_ui_test("set_device_state", device=device_name, power_on=True)
    obs_world.wait_for(lambda: filter_power_ptzsim.power() is True, timeout=10)


def test_a_camera_is_left_alone_unless_it_is_set_to_be_powered(obs_world, no_power_ptzsim):
    # It was off when OBS started, and nothing turned it on
    assert no_power_ptzsim.power() is False

    # Turned on, it is not turned off when OBS closes, which teardown checks
    device_name = obs_world.device_names["visca-tcp-no-power"]
    obs_world.run_ui_test("set_device_state", device=device_name, power_on=True)
    obs_world.wait_for(lambda: no_power_ptzsim.power() is True, timeout=10)
    no_power_ptzsim.left_on = True
