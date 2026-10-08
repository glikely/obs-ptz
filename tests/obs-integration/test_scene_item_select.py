"""Covers selecting a source in the Sources dock also selecting its camera in
the PTZ dock, so that with several cameras in one scene the one being
worked on is the one the dock controls. It follows the same autoselect
setting as a change of scene does.
"""

import json
import time

from test_filter_devices import camera_sim, cameras  # noqa: F401


def selected(world, device, out):
    if out.exists():
        out.unlink()
    world.run_ui_test("get_dock_selection", device=device, filename=str(out))
    world.wait_for(out.exists)
    return json.loads(out.read_text())["selected"]


def two_cameras_in_a_scene(world, cameras, tmp_path):  # noqa: F811
    scene = world.create_scene()
    for name in ("select-cam-a", "select-cam-b"):
        cameras.add_source(scene, name)
        cameras.add_filter(name)
        world.wait_for_device_by_name(name, tmp_path / f"{name}.json", lambda r: r["found"] and r["bound"])
    world.ws.call("SetCurrentProgramScene", {"sceneName": scene})
    return scene


def test_selecting_a_source_selects_its_camera(obs_world, cameras, tmp_path):  # noqa: F811
    scene = two_cameras_in_a_scene(obs_world, cameras, tmp_path)
    out = tmp_path / "selection.json"

    obs_world.run_ui_test("select_scene_item", scene=scene, source="select-cam-b", autoselect="true")
    obs_world.wait_for(lambda: selected(obs_world, "select-cam-b", out))

    obs_world.run_ui_test("select_scene_item", scene=scene, source="select-cam-a")
    obs_world.wait_for(lambda: selected(obs_world, "select-cam-a", out))


def test_it_follows_the_autoselect_setting(obs_world, cameras, tmp_path):  # noqa: F811
    scene = two_cameras_in_a_scene(obs_world, cameras, tmp_path)
    out = tmp_path / "selection.json"
    obs_world.run_ui_test("select_scene_item", scene=scene, source="select-cam-a", autoselect="true")
    obs_world.wait_for(lambda: selected(obs_world, "select-cam-a", out))

    obs_world.run_ui_test("select_scene_item", scene=scene, source="select-cam-b", autoselect="false")
    # Nothing to wait for: give a queued selection, if there were one, time to run
    time.sleep(1)
    assert selected(obs_world, "select-cam-a", out)
