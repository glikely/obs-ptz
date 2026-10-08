"""Covers reordering the cameras, in the PTZ Controls dock and in the settings
dialog, which show the one list of cameras and so the one order.

The cameras are moved two ways, both through tests/ui-harness/reorder-cameras-
test.cpp's "reorder_cameras" test: with the model's moveRow(), and by doing to
a list view what a drop does (CircularListView::dropCurrentRowAt()), which is
what dragging a camera does once the mouse is let go. A drag itself can't be
made without a real pointer.

The order belongs to the scene collection, which a camera's filter is in: it is
saved in the collection's file, and is there again when the collection is loaded
again, as a restart does. The test for that loads the collection again by
switching away from it and back.
"""

import json
import os

import pytest


def scene_collection_orders(world):
    """The "ptz_camera_order" of each scene collection file, as uuids, by file"""
    scenes = world.config_dir.parent.parent / "basic" / "scenes"
    found = {}
    for path in scenes.glob("*.json"):
        modules = json.loads(path.read_text()).get("modules", {})
        found[path.name] = [item["uuid"] for item in modules.get("ptz_camera_order", [])]
    return found


@pytest.fixture
def cameras(obs_world, tmp_path):
    """The names of the cameras in the order they start in, put back that
    way afterwards so no other test sees them moved."""
    out = tmp_path / "order.json"
    original = obs_world.reorder_cameras(out)["devices"]
    assert len(original) > 3
    yield original
    obs_world.reorder_cameras(out, set_order=",".join(original))


@pytest.fixture
def dialog(obs_world, tmp_path):
    """The settings dialog, open, which has its own list of the cameras"""
    obs_world.run_ui_test("open_settings_dialog", device=obs_world.device_names["visca-tcp"])
    out = tmp_path / "dialog.json"
    obs_world.wait_for(lambda: obs_world.reorder_cameras(out)["settings_devices"])
    return out


def test_a_camera_moves_down(obs_world, tmp_path, cameras):
    result = obs_world.reorder_cameras(tmp_path / "o.json", **{"from": 0, "to": 3})

    assert result["moved"] is True
    assert result["devices"] == cameras[1:3] + [cameras[0]] + cameras[3:]


def test_a_camera_moves_up(obs_world, tmp_path, cameras):
    result = obs_world.reorder_cameras(tmp_path / "o.json", **{"from": 3, "to": 1})

    assert result["moved"] is True
    assert result["devices"] == [cameras[0], cameras[3]] + cameras[1:3] + cameras[4:]


def test_a_camera_moved_onto_its_own_place_is_not_moved(obs_world, tmp_path, cameras):
    for to in (2, 3):
        result = obs_world.reorder_cameras(tmp_path / "o.json", **{"from": 2, "to": to})
        assert result["moved"] is False
        assert result["devices"] == cameras


def test_a_camera_cannot_be_moved_off_the_ends(obs_world, tmp_path, cameras):
    n = len(cameras)
    for src, dst in ((n, 0), (-1, 0), (0, n + 1), (0, -1)):
        result = obs_world.reorder_cameras(tmp_path / "o.json", **{"from": src, "to": dst})
        assert result["moved"] is False
        assert result["devices"] == cameras


def test_dropping_a_camera_on_another_puts_it_after(obs_world, tmp_path, cameras):
    result = obs_world.reorder_cameras(tmp_path / "o.json", via="drop", **{"from": 0, "to": 2})

    assert result["moved"] is True
    assert result["devices"] == cameras[1:3] + [cameras[0]] + cameras[3:]
    assert result["selected_row"] == 2


def test_dropping_a_camera_past_the_last_puts_it_last(obs_world, tmp_path, cameras):
    result = obs_world.reorder_cameras(tmp_path / "o.json", via="drop", **{"from": 1, "to": len(cameras)})

    assert result["devices"] == [cameras[0]] + cameras[2:] + [cameras[1]]


def test_dropping_a_camera_where_it_is_does_nothing(obs_world, tmp_path, cameras):
    result = obs_world.reorder_cameras(tmp_path / "o.json", via="drop", **{"from": 2, "to": 2})

    assert result["moved"] is False
    assert result["devices"] == cameras


def test_the_cameras_can_be_dragged_in_the_dock_and_the_dialog(obs_world, tmp_path, dialog):
    result = obs_world.reorder_cameras(tmp_path / "o.json")

    assert result["dock_drag"] is True
    assert result["settings_drag"] is True


def test_dropping_a_camera_in_the_dialog_moves_it_in_the_dock_too(obs_world, tmp_path, cameras, dialog):
    result = obs_world.reorder_cameras(tmp_path / "o.json", **{"in": "settings"}, via="drop",
                                       **{"from": 0, "to": 2})

    expected = cameras[1:3] + [cameras[0]] + cameras[3:]
    assert result["moved"] is True
    assert result["devices"] == expected
    assert result["settings_devices"] == expected


def test_dropping_a_camera_in_the_dock_moves_it_in_the_dialog_too(obs_world, tmp_path, cameras, dialog):
    result = obs_world.reorder_cameras(tmp_path / "o.json", via="drop", **{"from": 3, "to": 0})

    # onto the lower half of the first, so after it
    expected = [cameras[0], cameras[3]] + cameras[1:3] + cameras[4:]
    assert result["devices"] == expected
    assert result["settings_devices"] == expected


def test_the_order_is_saved_with_the_scene_collection(obs_world, tmp_path, cameras):
    result = obs_world.reorder_cameras(tmp_path / "o.json", **{"from": 0, "to": len(cameras)})

    obs_world.wait_for(lambda: result["uuids"] in scene_collection_orders(obs_world).values(), timeout=15)


def saved_scene_collection(world, uuids):
    """The file of the scene collection whose saved order is uuids"""
    scenes = world.config_dir.parent.parent / "basic" / "scenes"
    for path in scenes.glob("*.json"):
        modules = json.loads(path.read_text()).get("modules", {})
        if [item["uuid"] for item in modules.get("ptz_camera_order", [])] == uuids:
            return path
    return None


def test_the_saved_order_is_what_loading_the_scene_collection_gives(obs_world, tmp_path, cameras):
    """The collection is not loaded again here, which takes the whole session's
    cameras down with it: its saved order is handed to what OBS hands it to when it
    loads one, after the cameras were put in another order, as a collection with another
    order would have them. (test_the_order_survives_loading_the_collection does it for real.)"""
    out = tmp_path / "o.json"
    moved = obs_world.reorder_cameras(out, **{"from": 0, "to": len(cameras)})
    expected = cameras[1:] + [cameras[0]]
    assert moved["devices"] == expected
    obs_world.wait_for(lambda: saved_scene_collection(obs_world, moved["uuids"]), timeout=15)
    saved = saved_scene_collection(obs_world, moved["uuids"])

    scrambled = obs_world.reorder_cameras(out, set_order=",".join(reversed(cameras)))
    assert scrambled["devices"] == list(reversed(cameras))

    loaded = obs_world.reorder_cameras(out, load_from=str(saved))
    assert loaded["devices"] == expected


def test_a_collection_with_no_saved_order_leaves_the_cameras_as_they_are(obs_world, tmp_path, cameras):
    empty = tmp_path / "empty.json"
    empty.write_text(json.dumps({"modules": {}}))

    result = obs_world.reorder_cameras(tmp_path / "o.json", load_from=str(empty))

    assert result["devices"] == cameras


@pytest.mark.skipif(not os.environ.get("PTZSIM_TEST_COLLECTION_RELOAD"),
                    reason="loading the collection again takes this session's cameras down with it, and has "
                    "OBS busy for longer than the tests wait: set PTZSIM_TEST_COLLECTION_RELOAD=1 to run it, "
                    "with only these tests (-k reorder_cameras)")
def test_the_order_survives_loading_the_collection(obs_world, tmp_path, cameras):
    moved = obs_world.reorder_cameras(tmp_path / "o.json", **{"from": 0, "to": len(cameras)})
    expected = cameras[1:] + [cameras[0]]
    assert moved["devices"] == expected

    out = tmp_path / "again.json"
    obs_world.reorder_cameras(out, reload="1")
    obs_world.wait_for(lambda: obs_world.reorder_cameras(out)["devices"] == expected, timeout=30)


def test_cameras_the_order_does_not_name_go_last_in_the_order_they_were(obs_world, tmp_path, cameras):
    result = obs_world.reorder_cameras(tmp_path / "o.json", set_order=cameras[3] + "," + cameras[1])

    assert result["devices"] == [cameras[3], cameras[1], cameras[0], cameras[2]] + cameras[4:]


def test_the_order_survives_restarting_obs(obs_world, tmp_path, cameras):
    """The whole of it: move a camera, quit OBS the way a user does, start it again on the
    same profile, and the cameras are where they were left"""
    out = tmp_path / "o.json"
    moved = obs_world.reorder_cameras(out, **{"from": 0, "to": len(cameras)})
    expected = cameras[1:] + [cameras[0]]
    assert moved["devices"] == expected

    obs_world.restart_obs()

    again = obs_world.reorder_cameras(out)
    assert again["devices"] == expected
    assert again["uuids"] == moved["uuids"]
