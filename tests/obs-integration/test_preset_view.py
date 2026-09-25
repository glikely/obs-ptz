"""Covers what the PTZ Controls dock's preset list shows when it has no
camera to show the presets of.

The preset list is a view onto the same model as the camera list, rooted at
the selected camera so that it lists that camera's presets. Rooted at an
invalid index, which is what it is with no camera selected, it shows the
model's top level instead, which is the list of cameras -- so the preset list
would show the cameras. The view also throws its root away, and the camera
list its selection, whenever the model resets, as it does whenever a device is
added or removed, so that has to be put right again then, too.

Driven through tests/ui-harness/preset-view-test.cpp's "get_preset_view"
test, see World.preset_view() in conftest.py.
"""

from conftest import write_preset_file


def cameras_listed(view):
    return [name for name in (r["text"] for r in view["rows"]) if name in {d["text"] for d in view["device_names"]}]


def test_preset_list_is_blank_with_no_camera_selected(obs_world, tmp_path):
    view = obs_world.preset_view(tmp_path / "view.json", select="none")

    assert view["found"] is True
    assert view["selected"] is False
    assert len(view["device_names"]) > 1
    assert view["rows"] == []


def test_preset_list_shows_the_selected_cameras_presets(obs_world, tmp_path):
    device_id = obs_world.device_ids["visca-tcp-flaky"]
    out = tmp_path / "view.json"
    presets = tmp_path / "presets.json"
    write_preset_file(presets, [{"id": 1, "name": "Kitchen"}, {"id": 2, "name": "Garden"}])
    obs_world.run_ui_test("import_presets", device_id=device_id, filename=str(presets))

    obs_world.preset_view(out, select=device_id)
    view = obs_world.wait_for_preset_view(out, lambda v: v["rows"])

    assert view["selected"] is True
    assert [r["text"] for r in view["rows"]] == ["Kitchen", "Garden"]


def test_preset_list_is_blank_again_once_the_selection_is_cleared(obs_world, tmp_path):
    out = tmp_path / "view.json"
    obs_world.preset_view(out, select=obs_world.device_ids["visca-tcp"])

    view = obs_world.preset_view(out, select="none")

    assert view["selected"] is False
    assert view["rows"] == []


def test_preset_list_is_blank_after_the_model_resets(obs_world, tmp_path):
    """Adding a device resets the model, which clears the camera list's
    selection without saying so, and the preset list's root."""
    out = tmp_path / "view.json"
    obs_world.preset_view(out, select=obs_world.device_ids["visca-tcp"])

    obs_world.preset_view(out, add_device="preset-view-extra")
    try:
        view = obs_world.wait_for_preset_view(out, lambda v: v["found"])
        assert "preset-view-extra" in {d["text"] for d in view["device_names"]}
        assert cameras_listed(view) == []
        assert view["selected"] is False
        assert view["rows"] == []
    finally:
        obs_world.preset_view(out, remove_device="preset-view-extra")
