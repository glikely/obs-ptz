"""Covers what the PTZ Controls dock's preset list shows when it has no
camera to show the presets of.

The preset list is a view onto the same model as the camera list, rooted at
the selected camera so that it lists that camera's presets. Rooted at an
invalid index, which is what it is with no camera selected, it shows the
model's top level instead, which is the list of cameras -- so the preset list
would show the cameras. The view also throws its root away, and the camera
list its selection, whenever the model resets, as it does whenever a device is
added or removed, so that has to be put right again then, too.

Driven through the UI driver, which reads the dock's preset list, see
World.preset_view() in conftest.py.
"""

from conftest import write_preset_file


def cameras_listed(view):
    return [name for name in (r["text"] for r in view["rows"]) if name in {d["text"] for d in view["device_names"]}]


def test_preset_list_is_blank_with_no_camera_selected(obs_world, tmp_path):
    out = tmp_path / "view.json"
    obs_world.preset_view(out, select="none")
    # A device the test before removed has the model reset, which the view
    # catches up with on a queued call
    view = obs_world.wait_for_preset_view(out, lambda v: v["rows"] == [])

    assert view["found"] is True
    assert view["selected"] is False
    assert len(view["device_names"]) > 1


def test_preset_list_shows_the_selected_cameras_presets(obs_world, tmp_path):
    device_name = obs_world.device_names["visca-tcp-flaky"]
    out = tmp_path / "view.json"
    presets = tmp_path / "presets.json"
    for _ in range(2):
        assert obs_world.create_preset(device_name)
    try:
        view = obs_world.preset_view(out, select=device_name)
        view = obs_world.wait_for_preset_view(out, lambda v: len(v["rows"]) == 2)
        # the camera keeps no names, so the file names them
        slots = [int(r["id"].partition(":")[2]) for r in view["rows"]]
        write_preset_file(presets, [{"id": slots[0], "name": "Kitchen"}, {"id": slots[1], "name": "Garden"}])
        obs_world.import_presets(device_name, presets)

        view = obs_world.wait_for_preset_view(out, lambda v: [r["text"] for r in v["rows"]] == ["Kitchen", "Garden"])
        assert view["selected"] is True
    finally:
        for row in obs_world.preset_view(out)["rows"]:
            obs_world.call_proc(device_name, "ptz_preset_delete", {"id": row["id"]})


def test_preset_list_is_blank_again_once_the_selection_is_cleared(obs_world, tmp_path):
    out = tmp_path / "view.json"
    obs_world.preset_view(out, select=obs_world.device_names["visca-tcp"])

    # What the test before set going can still choose a camera just after: clear
    # the selection until it stays clear, as test_preset_list_is_blank_with_no_camera_selected
    # waits for the view to catch up
    cleared = []

    def clear():
        cleared[:] = [obs_world.preset_view(out, select="none")]
        return cleared[0]["selected"] is False

    obs_world.wait_for(clear)
    assert cleared[0]["rows"] == []


def test_preset_list_is_blank_after_the_model_resets(obs_world, tmp_path):
    """Adding a device resets the model, which clears the camera list's
    selection without saying so, and the preset list's root."""
    out = tmp_path / "view.json"
    obs_world.preset_view(out, select=obs_world.device_names["visca-tcp"])

    # A device is a filter on a source, here a source in a scene of its own,
    # which OBS holds as it holds any other
    obs_world.ws.call("CreateInput", {
        "sceneName": obs_world.create_scene(),
        "inputName": "preset-view-extra",
        "inputKind": "ffmpeg_source",
        "inputSettings": {"is_local_file": True, "local_file": ""},
    })
    try:
        obs_world.ws.call("CreateSourceFilter", {
            "sourceName": "preset-view-extra",
            "filterName": "PTZ",
            "filterKind": "ca.secretlab.obs-ptz.visca",
            "filterSettings": {"type": "visca-over-ip", "host": "127.0.0.1"},
        })
        # The dock puts the preset list right in a call queued after the
        # reset, so a look between the two can still see the cameras
        view = obs_world.wait_for_preset_view(
            out, lambda v: "preset-view-extra" in {d["text"] for d in v["device_names"]} and not cameras_listed(v))
        assert view["selected"] is False
        assert view["rows"] == []
    finally:
        obs_world.ws.call("RemoveInput", {"inputName": "preset-view-extra"})
