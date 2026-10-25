"""Covers selecting a source in the Sources dock also selecting its camera in
the PTZ dock, so that with several cameras in one scene the one being
worked on is the one the dock controls. It follows the same autoselect
setting as a change of scene does.
"""

import itertools
import time

from test_filter_devices import camera_sim, cameras  # noqa: F401

# Each test's sources have names of their own. OBS 32.0 can keep an audio
# source alive after RemoveInput (its audio mixer holds on to it), and the
# name with it, so a name used again would find the last test's source still
# there.
_scene_numbers = itertools.count(1)


def two_cameras_in_a_scene(world, cameras, tmp_path):  # noqa: F811
    """Two cameras in the program scene, with autoselect on, and their names.
    The scene change is announced to the dock after the request returns, and
    selects the first camera, so wait for that before the test selects
    anything: it would otherwise undo the test's own selection."""
    scene = world.create_scene()
    number = next(_scene_numbers)
    names = (f"select-cam-a-{number}", f"select-cam-b-{number}")
    for name in names:
        cameras.add_source(scene, name)
        cameras.add_filter(name)
        world.wait_for_device_by_name(name, tmp_path / f"{name}.json", lambda r: r["found"] and r["bound"])
    world.set_autoselect(True)
    world.ws.call("SetCurrentProgramScene", {"sceneName": scene})
    world.wait_for(lambda: world.dock_selected(names[0]))
    return names


def test_selecting_a_source_selects_its_camera(obs_world, cameras, tmp_path):  # noqa: F811
    a, b = two_cameras_in_a_scene(obs_world, cameras, tmp_path)

    obs_world.select_scene_item(b)
    obs_world.wait_for(lambda: obs_world.dock_selected(b))

    obs_world.select_scene_item(a)
    obs_world.wait_for(lambda: obs_world.dock_selected(a))


def test_it_follows_the_autoselect_setting(obs_world, cameras, tmp_path):  # noqa: F811
    a, b = two_cameras_in_a_scene(obs_world, cameras, tmp_path)
    obs_world.select_scene_item(a)
    obs_world.wait_for(lambda: obs_world.dock_selected(a))

    obs_world.set_autoselect(False)
    obs_world.select_scene_item(b)
    # Nothing to wait for: give a queued selection, if there were one, time to run
    time.sleep(1)
    assert obs_world.dock_selected(a)
