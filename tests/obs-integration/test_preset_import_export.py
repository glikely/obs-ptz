"""End-to-end tests for the preset export/import feature (issue #78),
driven entirely through the REAL "Export/Import Presets..." context-menu
actions (on_actionPresetExport_triggered()/on_actionPresetImport_
triggered() in src/ptz-controls.cpp) -- deliberately the only way this
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

This drives tests/ui-harness/'s own "export_presets"/"import_presets"
tests (tests/ui-harness/preset-export-import-test.cpp) over the "obs-ptz"
vendor's "ui_test_run" request every other tests/ui-harness/ test uses
(see World.run_ui_test() in conftest.py). The one thing it still can't drive is the
real QFileDialog itself: see on_actionPresetExport_triggered()'s own
comment in ptz-controls.cpp for why (native OS file pickers aren't
reachable from outside the process) and how it's bypassed for this
(PTZ_UI_TEST_PRESET_EXPORT_FILE/_IMPORT_FILE, only under an
-DENABLE_UI_TESTS=ON build).

Each test has a camera of its own (test_visca_user_profiles.py's), which has
both stores and keeps no names.

run_ui_test() is fire-and-forget (the harness dispatch is queued onto
OBS's GUI thread, not run synchronously -- see World.run_ui_test()'s own
docstring), so assertions here wait for the file or the list to change.

Deliberately not covered here: feeding a bad/missing file to the import
action. Going through the real action means a bad file pops a real, modal
QMessageBox::warning() in the live OBS window -- nothing in this suite
clicks it away, so triggering that on purpose would hang the GUI thread
(and therefore every subsequent queued test) rather than fail cleanly.
"""

import json

from conftest import write_preset_file, write_preset_file_v2
from test_visca_user_profiles import Camera

MODEL = "0123:0001"


def preset_list(world, camera):
    return world.call_proc(camera.device_name, "ptz_preset_get_list", returns="data")["return"]


def listed(world, camera):
    """The presets in display order, as (id, store, name)"""
    info = preset_list(world, camera)
    return [(e["id"], info["presets"][e["id"]]["store"], info["presets"][e["id"]]["name"]) for e in info["order"]]


def create(world, camera, name, store):
    result = world.call_proc(camera.device_name, "ptz_preset_create", {"name": name, "store": store}, returns="string")
    assert result["return"], f"no {store} preset was made"
    return result["return"]


def update(world, camera, preset_id, **changes):
    world.call_proc(camera.device_name, "ptz_preset_update", {"id": preset_id, "changes": changes})


def values_of(world, camera, preset_id):
    return preset_list(world, camera)["presets"][preset_id]["values"]


def export(world, camera, path):
    path.unlink(missing_ok=True)
    world.run_ui_test("export_presets", device=camera.device_name, filename=str(path))
    world.wait_for(lambda: path.exists() and path.read_text())
    return json.loads(path.read_text())


def do_import(world, camera, path):
    world.run_ui_test("import_presets", device=camera.device_name, filename=str(path))


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


def test_export_unknown_device_leaves_no_file(obs_world, tmp_path):
    out_file = tmp_path / "should-not-exist.json"
    obs_world.run_ui_test("export_presets", device=999999, filename=str(out_file))

    # There's no success/failure signal to wait on here (device_name
    # 999999 has no deviceList row to select, so runPresetIOTest() in
    # tests/ui-harness/preset-export-import-test.cpp bails out and logs,
    # never reaching the file write) -- just give any queued dispatch a
    # moment to run, then confirm nothing appeared.
    try:
        obs_world.wait_for(out_file.exists, timeout=1)
    except AssertionError:
        pass
    assert not out_file.exists()
