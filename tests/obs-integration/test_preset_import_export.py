"""End-to-end tests for the preset export/import feature (issue #78),
driven entirely through the REAL "Export/Import Presets..." context-menu
actions (on_actionPresetExport_triggered()/on_actionPresetImport_
triggered() in src/controller/ptz-controls.cpp) -- deliberately the only way this
feature is tested; there is no lower-level entry point that bypasses the
real device-selection and menu-action wiring.

The actions read and write through the PTZ API's presets (docs/ptz-device-api.md,
"Presets"), so what is checked here is the API's too:

- a format 2 file has, in display order, each local preset in full (its name
  and values) and the name of each camera preset, with no thumbnails;
- an import replaces the device's local presets with those of the file, and
  sets the names of the camera presets it has that the file names, when the
  camera does not keep names;
- a format 1 file, from an older version, names camera presets by their slot.
- a file that isn't one of presets changes nothing, and says so.

This drives the dock through the UI driver (World.export_presets() and
World.import_presets() in conftest.py): it selects the device in the dock's
camera list, then calls the slot the action triggers with the file a user
would have chosen. The one thing it can't drive is the file dialog itself,
which is the platform's own, and not a Qt widget.

Each test has a camera of its own (test_visca_user_profiles.py's), which has
both stores and keeps no names.

The slot is called once the driver has answered, so assertions here wait for
the file or the list to change.
"""

from conftest import OBS_MAIN, write_preset_file, write_preset_file_v2
from test_visca_user_profiles import Camera

MODEL = "0123:0001"


def preset_list(world, camera):
    return world.call_proc(camera.device_name, "ptz_preset_get_list", returns="data")["return"]


def listed(world, camera):
    """The presets in display order, as (id, store, name)"""
    info = preset_list(world, camera)
    return [(e["id"], info["presets"][e["id"]]["store"], info["presets"][e["id"]]["name"]) for e in info["order"]]


def create(world, camera, name, store):
    preset_id = world.create_preset(camera.device_name, name, store)
    assert preset_id, f"no {store} preset was made"
    return preset_id


def update(world, camera, preset_id, **changes):
    world.call_proc(camera.device_name, "ptz_preset_update", {"id": preset_id, "changes": changes})


def values_of(world, camera, preset_id):
    return preset_list(world, camera)["presets"][preset_id]["values"]


def export(world, camera, path):
    return world.export_presets(camera.device_name, path)


def do_import(world, camera, path):
    world.import_presets(camera.device_name, path)


def test_export_writes_local_presets_in_full_and_camera_names(request, obs_world, tmp_path):
    camera = Camera(request, obs_world, tmp_path, MODEL)
    local = create(obs_world, camera, "Wide Shot", "local")
    update(obs_world, camera, local, values={"pan": 0.3, "tilt": -0.2, "zoom": 0.4})
    on_camera = create(obs_world, camera, "Stage", "camera")

    data = export(obs_world, camera, tmp_path / "exported.json")

    assert data["obs-ptz-preset-format"] == 2
    assert data["device"] == camera.source
    assert [(p["id"], p["store"], p["name"]) for p in data["presets"]] == [
        (local, "local", "Wide Shot"), (on_camera, "camera", "Stage")]
    saved = data["presets"][0]["values"]
    assert (saved["pan"], saved["tilt"], saved["zoom"]) == (0.3, -0.2, 0.4)
    # what a camera keeps of a preset is not in the file, nor is a thumbnail
    assert "values" not in data["presets"][1]
    assert all("thumbnail" not in p for p in data["presets"])


def test_import_replaces_the_local_presets_and_keeps_the_order(request, obs_world, tmp_path):
    camera = Camera(request, obs_world, tmp_path, MODEL)
    first = create(obs_world, camera, "First", "local")
    update(obs_world, camera, first, values={"pan": 0.5, "tilt": 0.0})
    on_camera = create(obs_world, camera, "Stage", "camera")
    second = create(obs_world, camera, "Second", "local")
    update(obs_world, camera, second, values={"pan": -0.5, "tilt": 0.25})
    export(obs_world, camera, tmp_path / "exported.json")
    before = [name for _, _, name in listed(obs_world, camera)]
    assert before == ["First", "Stage", "Second"]

    # what has changed since, in both stores
    create(obs_world, camera, "Extra", "local")
    update(obs_world, camera, first, name="Renamed")
    update(obs_world, camera, on_camera, name="Another name")

    do_import(obs_world, camera, tmp_path / "exported.json")
    obs_world.wait_for(lambda: [name for _, _, name in listed(obs_world, camera)] == before, timeout=10)

    # the local presets were made again, with the values of the file
    now = listed(obs_world, camera)
    assert [store for _, store, _ in now] == ["local", "camera", "local"]
    assert now[1][0] == on_camera
    assert values_of(obs_world, camera, now[0][0])["pan"] == 0.5
    assert values_of(obs_world, camera, now[2][0])["tilt"] == 0.25
    # made again, so with ids of their own
    assert now[0][0] != first and now[2][0] != second


def test_import_copies_presets_to_a_different_device(request, obs_world, tmp_path):
    """The whole point of issue #78: presets should be portable between
    cameras/installs, not tied to the exporting device."""
    source = Camera(request, obs_world, tmp_path, MODEL)
    create(obs_world, source, "Kitchen", "local")
    exported = tmp_path / "exported.json"
    export(obs_world, source, exported)

    target = Camera(request, obs_world, tmp_path, MODEL)
    do_import(obs_world, target, exported)
    obs_world.wait_for(lambda: [name for _, _, name in listed(obs_world, target)] == ["Kitchen"], timeout=10)
    assert [store for _, store, _ in listed(obs_world, target)] == ["local"]
    # "device" records who exported, not who imported
    assert export(obs_world, target, tmp_path / "again.json")["device"] == target.source


def test_import_names_the_camera_presets_it_has(request, obs_world, tmp_path):
    camera = Camera(request, obs_world, tmp_path, MODEL)
    on_camera = create(obs_world, camera, "", "camera")
    other = create(obs_world, camera, "Other", "camera")
    path = tmp_path / "names.json"
    write_preset_file_v2(path, [
        {"id": on_camera, "store": "camera", "name": "Named by the file"},
        {"id": "camera:99", "store": "camera", "name": "Not on this camera"},
    ])

    do_import(obs_world, camera, path)

    obs_world.wait_for(lambda: dict((i, n) for i, _, n in listed(obs_world, camera)).get(on_camera)
                       == "Named by the file", timeout=10)
    now = listed(obs_world, camera)
    # a preset the file does not name keeps its name, and one the camera does not have is not made
    assert dict((i, n) for i, _, n in now)[other] == "Other"
    assert [i for i, _, _ in now] == [on_camera, other]


def test_import_reads_a_format_1_file(request, obs_world, tmp_path):
    camera = Camera(request, obs_world, tmp_path, MODEL)
    on_camera = create(obs_world, camera, "", "camera")
    slot = int(on_camera.partition(":")[2])
    path = tmp_path / "old.json"
    write_preset_file(path, [{"id": slot, "name": "Kitchen"}])

    do_import(obs_world, camera, path)

    obs_world.wait_for(lambda: dict((i, n) for i, _, n in listed(obs_world, camera)).get(on_camera) == "Kitchen",
                       timeout=10)



def test_a_file_that_is_not_presets_is_not_imported(request, obs_world, tmp_path):
    """It says so in a message box, which a user closes, and leaves the presets"""
    camera = Camera(request, obs_world, tmp_path, MODEL)
    local = create(obs_world, camera, "Keep", "local")
    path = tmp_path / "not-presets.json"
    path.write_text('{"something": "else"}')

    do_import(obs_world, camera, path)

    warning = (OBS_MAIN, {"class": "QMessageBox"})
    obs_world.wait_for(lambda: obs_world.ui.find(*warning))
    obs_world.ui.invoke(*warning, {"class": "QPushButton"}, method="click")
    assert listed(obs_world, camera) == [(local, "local", "Keep")]
