"""Covers selecting a source in the Sources dock also selecting its camera in
the PTZ dock, so that with several cameras in one scene the one being
worked on is the one the dock controls. It follows the same autoselect
setting as a change of scene does.
"""

import time

from test_filter_devices import camera_sim, cameras  # noqa: F401


def two_cameras_in_a_scene(world, cameras, tmp_path):  # noqa: F811
    """Two cameras in the program scene, with autoselect on. The scene change
    is announced to the dock after the request returns, and selects the first
    camera, so wait for that before the test selects anything: it would
    otherwise undo the test's own selection."""
    scene = world.create_scene()
    for name in ("select-cam-a", "select-cam-b"):
        cameras.add_source(scene, name)
        cameras.add_filter(name)
        world.wait_for_device_by_name(name, tmp_path / f"{name}.json", lambda r: r["found"] and r["bound"])
    world.set_autoselect(True)
    world.ws.call("SetCurrentProgramScene", {"sceneName": scene})
    world.wait_for(lambda: world.dock_selected("select-cam-a"))
    return scene


def test_selecting_a_source_selects_its_camera(obs_world, cameras, tmp_path):  # noqa: F811
    two_cameras_in_a_scene(obs_world, cameras, tmp_path)

    obs_world.select_scene_item("select-cam-b")
    obs_world.wait_for(lambda: obs_world.dock_selected("select-cam-b"))

    obs_world.select_scene_item("select-cam-a")
    obs_world.wait_for(lambda: obs_world.dock_selected("select-cam-a"))


def test_it_follows_the_autoselect_setting(obs_world, cameras, tmp_path):  # noqa: F811
    two_cameras_in_a_scene(obs_world, cameras, tmp_path)
    obs_world.select_scene_item("select-cam-a")
    obs_world.wait_for(lambda: obs_world.dock_selected("select-cam-a"))

    obs_world.set_autoselect(False)
    obs_world.select_scene_item("select-cam-b")
    # Nothing to wait for: give a queued selection, if there were one, time to run
    time.sleep(1)
    assert obs_world.dock_selected("select-cam-a")
