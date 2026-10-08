"""The PTZ Controls dock's presets with the two stores of the PTZ API (see
docs/ptz-device-api.md, "Presets"): the camera's, and the local one that any
device that can go to a position has.

Each test has a camera of its own (test_visca_user_profiles.py's): a VISCA one
with both stores, and one whose command set takes away memory_recall, so that
it has only the local store.

- every preset row says its store, and its tooltip where it is kept;
- the Add button has a menu of the stores only for a device that has both, a
  click uses the store chosen last, and that is saved with the dock's settings;
- a device with only the local store gets the dock's presets: add, recall and
  save.

Driven through tests/ui-harness/preset-view-test.cpp's "get_preset_view" and
"add_preset", see World.preset_view() in conftest.py.
"""

import json

from test_visca_user_profiles import Camera

BOTH = "0123:0001"
LOCAL_ONLY = "0123:0008"


def select(world, camera, tmp_path, **params):
    """The dock's view with the camera selected, once it has the stores it will have"""
    out = tmp_path / "view.json"
    world.preset_view(out, select=camera.device_name)
    return world.wait_for_preset_view(out, lambda v: v["selected"] and v["add_stores"], timeout=10)


def view(world, tmp_path):
    return world.preset_view(tmp_path / "view.json")


def wait_for_rows(world, tmp_path, count):
    return world.wait_for_preset_view(tmp_path / "view.json", lambda v: len(v["rows"]) == count)["rows"]


def restore_default(world, tmp_path):
    """The dock is the suite's, shared: the default a test changed is put back, with
    the camera still selected"""
    out = tmp_path / "default.json"
    view = world.preset_view(out)
    if view["add_menu"] and not view["add_menu"][2]["checked"]:
        world.preset_view(out, add="toggle_default")


def test_a_device_with_both_stores_adds_to_either(request, obs_world, tmp_path):
    camera = Camera(request, obs_world, tmp_path, BOTH)
    shown = select(obs_world, camera, tmp_path)
    assert [s["text"] for s in shown["add_stores"]] == ["camera", "local"]
    # the camera's, OBS's, and the setting of which the button uses (the wording of the first two is the locale's)
    assert len(shown["add_menu"]) == 3
    assert shown["add_menu"][2]["text"] == "Use Camera Saved Presets by Default"

    obs_world.run_ui_test("add_preset", device=camera.device_name, store="camera")
    obs_world.run_ui_test("add_preset", device=camera.device_name, store="local")
    rows = wait_for_rows(obs_world, tmp_path, 2)
    assert [r["store"] for r in rows] == ["camera", "local"]
    assert rows[0]["id"].startswith("camera:")
    assert rows[1]["id"].startswith("local:")
    assert rows[0]["tooltip"].lower().startswith("camera preset ")
    assert rows[1]["tooltip"] == "Saved in OBS"


def test_the_add_button_uses_the_default_store(request, obs_world, tmp_path):
    """The menu's entries add to a store, and the setting in it is where the button's click does"""
    camera = Camera(request, obs_world, tmp_path, BOTH)
    shown = select(obs_world, camera, tmp_path)
    # the camera's store is the default, which the setting says: the third entry
    assert [e["checked"] for e in shown["add_menu"]][2] is True
    out = tmp_path / "view.json"
    try:
        # choosing a store in the menu is for that preset, and not what the button does
        obs_world.preset_view(out, add="local")
        wait_for_rows(obs_world, tmp_path, 1)
        obs_world.preset_view(out, add="button")
        rows = wait_for_rows(obs_world, tmp_path, 2)
        assert [r["store"] for r in rows] == ["local", "camera"]

        obs_world.preset_view(out, add="toggle_default")
        view = obs_world.wait_for_preset_view(out, lambda v: v["add_menu"] and not v["add_menu"][2]["checked"])
        assert len(view["add_menu"]) == 3
        obs_world.preset_view(out, add="button")
        rows = wait_for_rows(obs_world, tmp_path, 3)
        assert [r["store"] for r in rows] == ["local", "camera", "local"]
    finally:
        restore_default(obs_world, tmp_path)


def test_the_default_store_is_saved_with_the_dock(request, obs_world, tmp_path):
    camera = Camera(request, obs_world, tmp_path, BOTH)
    select(obs_world, camera, tmp_path)
    config = obs_world.config_dir / "config.json"

    def saved():
        config.unlink(missing_ok=True)
        obs_world.run_ui_test("save_dock")
        obs_world.wait_for(config.exists)
        return json.loads(config.read_text())["preset_default_camera"]

    try:
        assert saved() is True
        obs_world.preset_view(tmp_path / "view.json", add="toggle_default")
        assert saved() is False
        obs_world.preset_view(tmp_path / "view.json", add="toggle_default")
        assert saved() is True
    finally:
        restore_default(obs_world, tmp_path)


def test_a_device_with_one_store_has_no_menu(request, obs_world, tmp_path):
    camera = Camera(request, obs_world, tmp_path, LOCAL_ONLY)
    shown = select(obs_world, camera, tmp_path)
    assert [s["text"] for s in shown["add_stores"]] == ["local"]
    assert shown["add_menu"] == []

    # whatever was chosen last, the button adds to the store it has
    obs_world.preset_view(tmp_path / "view.json", add="button")
    rows = wait_for_rows(obs_world, tmp_path, 1)
    assert [r["store"] for r in rows] == ["local"]


def test_a_device_with_only_local_presets_has_the_docks_presets(request, obs_world, tmp_path):
    camera = Camera(request, obs_world, tmp_path, LOCAL_ONLY)
    camera.wait_for_state(lambda s: s.get("features", {}).get("presets") is True and "pan" in s)

    # add: from where the camera is
    obs_world.run_ui_test("move_device", device=camera.device_name, mode="abs", pan=0.5, tilt=0.0)
    obs_world.wait_for(lambda: camera.sim.state()["pan"] > 0.1, timeout=10)
    camera.wait_for_state(lambda s: s.get("pan", 0) > 0.4)
    select(obs_world, camera, tmp_path)
    obs_world.preset_view(tmp_path / "view.json", add="button")
    preset = wait_for_rows(obs_world, tmp_path, 1)[0]["id"]

    # recall: goes back to it from elsewhere
    obs_world.run_ui_test("move_device", device=camera.device_name, mode="abs", pan=-0.5, tilt=0.0)
    obs_world.wait_for(lambda: camera.sim.state()["pan"] < -0.1, timeout=10)
    obs_world.call_proc(camera.device_name, "ptz_preset_recall", {"id": preset})
    obs_world.wait_for(lambda: camera.sim.state()["pan"] > 0.1, timeout=10)

    # save: replaces what it holds with where the camera is now
    obs_world.run_ui_test("move_device", device=camera.device_name, mode="abs", pan=-0.5, tilt=0.0)
    obs_world.wait_for(lambda: camera.sim.state()["pan"] < -0.1, timeout=10)
    camera.wait_for_state(lambda s: s.get("pan", 0) < -0.4)
    obs_world.call_proc(camera.device_name, "ptz_preset_save", {"id": preset})
    obs_world.run_ui_test("move_device", device=camera.device_name, mode="abs", pan=0.5, tilt=0.0)
    obs_world.wait_for(lambda: camera.sim.state()["pan"] > 0.1, timeout=10)
    obs_world.call_proc(camera.device_name, "ptz_preset_recall", {"id": preset})
    obs_world.wait_for(lambda: camera.sim.state()["pan"] < -0.1, timeout=10)


def local_preset_values(world, camera, preset):
    return world.call_proc(camera.device_name, "ptz_preset_get_list", returns="data")["return"]["presets"][preset]["values"]


def go_to(world, camera, pan):
    world.run_ui_test("move_device", device=camera.device_name, mode="abs", pan=pan, tilt=0.0)
    camera.wait_for_state(lambda s: abs(s.get("pan", 9) - pan) < 0.05)


def test_a_preset_hotkey_acts_on_that_row(request, obs_world, tmp_path):
    """Hotkey N is the N-th preset in the list, whichever store it is in"""
    camera = Camera(request, obs_world, tmp_path, LOCAL_ONLY)
    camera.wait_for_state(lambda s: s.get("features", {}).get("presets") is True and "pan" in s)
    out = tmp_path / "view.json"
    select(obs_world, camera, tmp_path)

    go_to(obs_world, camera, 0.5)
    obs_world.preset_view(out, add="button")
    go_to(obs_world, camera, -0.5)
    obs_world.preset_view(out, add="button")
    first, second = [r["id"] for r in wait_for_rows(obs_world, tmp_path, 2)]

    go_to(obs_world, camera, 0.0)
    obs_world.fire_hotkey("PTZ.Recall2")
    obs_world.wait_for(lambda: camera.sim.state()["pan"] < -0.1, timeout=10)
    obs_world.fire_hotkey("PTZ.Recall1")
    obs_world.wait_for(lambda: camera.sim.state()["pan"] > 0.1, timeout=10)

    go_to(obs_world, camera, 0.8)
    obs_world.fire_hotkey("PTZ.Save2")
    obs_world.wait_for(lambda: abs(local_preset_values(obs_world, camera, second)["pan"] - 0.8) < 0.05)
    assert abs(local_preset_values(obs_world, camera, first)["pan"] - 0.5) < 0.05


def test_the_preset_hotkeys_say_preset(obs_world):
    assert obs_world.hotkey_description("PTZ.Recall3") == "Preset Recall #3"
    assert obs_world.hotkey_description("PTZ.Save3") == "Preset Save #3"
