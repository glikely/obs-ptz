"""End-to-end tests for the preset export/import feature (issue #78),
driven entirely through the REAL "Export/Import Presets..." context-menu
actions (on_actionPresetExport_triggered()/on_actionPresetImport_
triggered() in src/ptz-controls.cpp) -- deliberately the only way this
feature is tested; there is no lower-level entry point that bypasses the
real device-selection and menu-action wiring.

This drives tests/ui-harness/'s own "export_presets"/"import_presets"
tests (tests/ui-harness/preset-export-import-test.cpp) over the "obs-ptz"
vendor's "ui_test_run" request every other tests/ui-harness/ test uses
(see World.run_ui_test() in conftest.py) -- which selects a device in the
real deviceList view and triggers the real actionPresetExport/
actionPresetImport QAction. The one thing it still can't drive is the
real QFileDialog itself: see on_actionPresetExport_triggered()'s own
comment in ptz-controls.cpp for why (native OS file pickers aren't
reachable from outside the process) and how it's bypassed for this
(PTZ_UI_TEST_PRESET_EXPORT_FILE/_IMPORT_FILE, only under an
-DENABLE_UI_TESTS=ON build).

Needs a build with -DENABLE_UI_TESTS=ON; conftest.py's obs_world fixture
always sets PTZ_UI_TEST_HARNESS=1, which is what actually activates the
harness (see src/ptz.c/ptz.h) and is a no-op if the binary wasn't built
with that option.

run_ui_test() is fire-and-forget (the harness dispatch is queued onto
OBS's GUI thread, not run synchronously -- see World.run_ui_test()'s own
docstring), so assertions here go through World.export_and_wait() rather
than checking a result immediately after triggering an action.

Deliberately not covered here: feeding a bad/missing file to the import
action. Unlike the vendor-request path this suite used to also have,
going through the real action means a bad file pops a real, modal
QMessageBox::warning() in the live OBS window -- nothing in this suite
clicks it away, so triggering that on purpose would hang the GUI thread
(and therefore every subsequent queued test) rather than fail cleanly.
"""

import json

from conftest import write_preset_file


def test_export_writes_expected_file(obs_world, tmp_path):
    device_id = obs_world.device_ids["visca-tcp"]
    seed = tmp_path / "seed.json"
    write_preset_file(seed, [{"id": 1, "name": "Wide Shot"}], preset_max=8)
    obs_world.run_ui_test("import_presets", device_id=device_id, filename=str(seed))

    out_file = tmp_path / "exported.json"
    data = obs_world.export_and_wait(device_id, out_file, [{"id": 1, "name": "Wide Shot"}])

    assert data["obs-ptz-preset-format"] == 1
    assert data["device"] == "sim-visca-tcp"
    assert data["preset_max"] == 8


def test_import_replaces_rather_than_merges(obs_world, tmp_path):
    """importPresets() clears the existing preset map first (see
    PTZDevice::importPresets() in src/ptz-device.cpp), so a second import
    must fully replace, not merge with, whatever the first one left
    behind."""
    device_id = obs_world.device_ids["visca-serial"]

    first = tmp_path / "first.json"
    write_preset_file(first, [{"id": 1, "name": "Before"}])
    obs_world.run_ui_test("import_presets", device_id=device_id, filename=str(first))

    second = tmp_path / "second.json"
    write_preset_file(second, [{"id": 2, "name": "After"}], preset_max=4)
    obs_world.run_ui_test("import_presets", device_id=device_id, filename=str(second))

    reexported = tmp_path / "reexported.json"
    data = obs_world.export_and_wait(device_id, reexported, [{"id": 2, "name": "After"}])
    assert data["preset_max"] == 4


def test_import_copies_presets_to_a_different_device(obs_world, tmp_path):
    """The whole point of issue #78: presets should be portable between
    cameras/installs, not tied to the exporting device."""
    src_id = obs_world.device_ids["visca-tcp"]
    dst_id = obs_world.device_ids["pelco-p"]

    seed = tmp_path / "seed.json"
    write_preset_file(seed, [{"id": 1, "name": "Wide Shot"}], preset_max=16)
    obs_world.run_ui_test("import_presets", device_id=src_id, filename=str(seed))

    export_file = tmp_path / "src.json"
    src_data = obs_world.export_and_wait(src_id, export_file, [{"id": 1, "name": "Wide Shot"}])

    obs_world.run_ui_test("import_presets", device_id=dst_id, filename=str(export_file))

    dst_export = tmp_path / "dst.json"
    dst_data = obs_world.export_and_wait(dst_id, dst_export, [{"id": 1, "name": "Wide Shot"}])

    assert dst_data["presets"] == src_data["presets"]
    assert dst_data["preset_max"] == src_data["preset_max"]
    # "device" records who exported, not who imported -- informational
    # metadata that import never applies back.
    assert dst_data["device"] == "sim-pelco-p"


def test_export_unknown_device_leaves_no_file(obs_world, tmp_path):
    out_file = tmp_path / "should-not-exist.json"
    obs_world.run_ui_test("export_presets", device_id=999999, filename=str(out_file))

    # There's no success/failure signal to wait on here (device_id
    # 999999 has no deviceList row to select, so runPresetIOTest() in
    # tests/ui-harness/preset-export-import-test.cpp bails out and logs,
    # never reaching the file write) -- just give any queued dispatch a
    # moment to run, then confirm nothing appeared.
    try:
        obs_world.wait_for(out_file.exists, timeout=1)
    except AssertionError:
        pass
    assert not out_file.exists()
