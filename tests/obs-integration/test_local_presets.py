"""Covers local presets: presets that OBS stores rather than the camera,
which save the device's state as it reports it, and recall only the values
the user chose (PTZDevice::saveLocalPreset()/recallLocalPreset() in
src/ptz-device.cpp).

A local preset is saved and recalled by the same ptz_preset_save/
ptz_preset_recall a camera preset is, from a ptz_action_source here, and
lives alongside the device's camera presets. Presets are put on the device
through the dock's real Import Presets action, added through its real Add
Local Preset action, and edited through the real Edit Preset dialog, driven
by tests/ui-harness/preset-local-test.cpp.
"""

import json
import time

import pytest

from conftest import write_preset_file

ACTION_PRESET_RECALL = 2
ACTION_PAN_TILT = 3
ACTION_STOP = 4
ACTION_PRESET_SAVE = 5

LOCAL_ID = 7
CAMERA_ID = 1

# Each with the state key a local preset saves where it pans to under: a VISCA
# camera's own position, which the movement API's "pan" can only approximate
# (see PTZVisca::presetStateKeys())
BACKENDS = [("visca-udp", "pan_pos"), ("onvif", "pan")]


@pytest.fixture(scope="module", autouse=True)
def _leave_the_camera_where_it_was(obs_world):
    """Every device here moves the one shared ptzsim camera. Put it back where
    the tests before these left it, for the tests after them: ONVIF's
    absolute moves are in ptzsim's own units."""
    start = obs_world.state()
    yield
    obs_world.run_ui_test("move_device", device_id=obs_world.device_ids["onvif"], mode="abs",
                          pan=start["pan"], tilt=start["tilt"], zoom=start["zoom"])
    obs_world.wait_for_state(lambda s: all(abs(s[k] - start[k]) < 0.01 for k in ("pan", "tilt", "zoom")))


def import_presets(obs_world, device_id, tmp_path, presets):
    path = tmp_path / "presets.json"
    write_preset_file(path, presets)
    obs_world.run_ui_test("import_presets", device_id=device_id, filename=str(path))


def preset(obs_world, device_id, preset_id, out_file):
    if out_file.exists():
        out_file.unlink()
    obs_world.run_ui_test("get_preset", device_id=device_id, preset_id=preset_id, filename=str(out_file))
    obs_world.wait_for(out_file.exists)
    return json.loads(out_file.read_text())


def wait_for_preset(obs_world, device_id, preset_id, out_file, predicate, timeout=5):
    last = None
    deadline = time.time() + timeout
    while time.time() < deadline:
        last = preset(obs_world, device_id, preset_id, out_file)
        if predicate(last):
            return last
        time.sleep(0.2)
    raise AssertionError(f"preset {preset_id} never matched; last seen: {last}")


def run_and_read(obs_world, out_file, cmd, **params):
    if out_file.exists():
        out_file.unlink()
    obs_world.run_ui_test(cmd, filename=str(out_file), **params)
    obs_world.wait_for(out_file.exists)
    return json.loads(out_file.read_text())


def move_and_stop(obs_world, device_id, pan_speed):
    obs_world.trigger_action(device_id, ACTION_PAN_TILT, pan_speed=pan_speed, tilt_speed=0.0)
    time.sleep(0.5)
    obs_world.trigger_action(device_id, ACTION_STOP)
    return obs_world.wait_for_state(lambda s: s["pan_speed"] == 0.0)


def toward_centre(obs_world):
    """A pan speed that moves the camera away from the limit nearest it: an
    earlier test can leave it at one, where a move toward it goes nowhere"""
    return -0.6 if obs_world.state()["pan"] > 0 else 0.6


def settled_device_pan(obs_world, device_id, out_file, before, key="pan_pos"):
    """The device's own "pan" once it has caught up with where the camera
    stopped: moved from `before`, and the same three readings running. A
    driver only reads the position back now and then, see PTZVisca's
    update_timer and PTZOnvif's status polling."""
    readings = []
    state = {}

    def settled():
        state.update(obs_world.device_state(device_id, out_file)["state"])
        readings.append(state.get(key))
        last = readings[-3:]
        return len(last) == 3 and last[0] is not None and last[0] != before and last.count(last[0]) == 3

    try:
        obs_world.wait_for(settled, timeout=10, interval=0.4)
    except AssertionError:
        raise AssertionError(f"device pan never settled away from {before}: {readings[-5:]}, "
                             f"connected={state.get('connected')}")
    return readings[-1]


@pytest.mark.parametrize("backend,key", BACKENDS)
def test_local_preset_saves_and_recalls_where_the_camera_was(obs_world, backend, key, tmp_path):
    device_id = obs_world.device_ids[backend]
    import_presets(obs_world, device_id, tmp_path, [
        {"id": CAMERA_ID, "name": "On camera"},
        {"id": LOCAL_ID, "name": "Local", "local": True},
    ])
    presets_before = set(obs_world.state().get("presets", {}))
    state_file = tmp_path / "state.json"
    speed = toward_centre(obs_world)
    before = obs_world.device_state(device_id, state_file)["state"].get(key)

    saved = move_and_stop(obs_world, device_id, speed)
    settled_device_pan(obs_world, device_id, state_file, before, key)

    obs_world.trigger_action(device_id, ACTION_PRESET_SAVE, preset_id=LOCAL_ID)
    info = wait_for_preset(obs_world, device_id, LOCAL_ID, tmp_path / "preset.json",
                           lambda p: key in p["preset"].get("state", {}))["preset"]
    assert info["local"] is True
    assert info["recall"][key] is True
    # Saved by OBS, not in the camera's own memory
    assert set(obs_world.state().get("presets", {})) == presets_before

    move_and_stop(obs_world, device_id, -speed)
    obs_world.wait_for_state(lambda s: abs(s["pan"] - saved["pan"]) > 0.05)

    obs_world.trigger_action(device_id, ACTION_PRESET_RECALL, preset_id=LOCAL_ID)
    obs_world.wait_for_state(lambda s: abs(s["pan"] - saved["pan"]) < 0.02, timeout=5)


def test_local_preset_recalls_only_the_chosen_values(obs_world, tmp_path):
    device_id = obs_world.device_ids["visca-udp"]
    state_file = tmp_path / "state.json"
    before = obs_world.device_state(device_id, state_file)["state"].get("pan_pos")
    move_and_stop(obs_world, device_id, toward_centre(obs_world))
    settled_device_pan(obs_world, device_id, state_file, before)
    tilt = obs_world.device_state(device_id, state_file)["state"]["tilt_pos"]

    import_presets(obs_world, device_id, tmp_path, [{
        "id": LOCAL_ID, "local": True,
        "state": {"pan_pos": -0x800, "tilt_pos": 0x200},
        "recall": {"pan_pos": True, "tilt_pos": False},
    }])
    wait_for_preset(obs_world, device_id, LOCAL_ID, tmp_path / "preset.json",
                    lambda p: p["preset"].get("recall", {}).get("tilt_pos") is False)

    obs_world.trigger_action(device_id, ACTION_PRESET_RECALL, preset_id=LOCAL_ID)
    after = obs_world.wait_for_device_state(
        device_id, state_file, lambda r: abs(r["state"].get("pan_pos", 0) + 0x800) <= 2, timeout=10)
    assert abs(after["state"]["tilt_pos"] - tilt) <= 2


def test_camera_presets_are_still_on_the_camera(obs_world, tmp_path):
    device_id = obs_world.device_ids["visca-udp"]
    import_presets(obs_world, device_id, tmp_path, [
        {"id": 3, "name": "On camera"},
        {"id": LOCAL_ID, "name": "Local", "local": True},
    ])
    wait_for_preset(obs_world, device_id, 3, tmp_path / "preset.json", lambda p: p["preset"].get("id") == 3)

    obs_world.trigger_action(device_id, ACTION_PRESET_SAVE, preset_id=3)
    obs_world.wait_for_state(lambda s: "3" in s.get("presets", {}))


def test_adding_a_local_preset_saves_the_state_it_can_recall(obs_world, tmp_path):
    device_id = obs_world.device_ids["visca-udp"]
    import_presets(obs_world, device_id, tmp_path, [])
    out = tmp_path / "add.json"
    obs_world.wait_for(lambda: run_and_read(obs_world, out, "get_preset", device_id=device_id, preset_id=0)
                       ["presets"] == [])

    added = run_and_read(obs_world, out, "add_preset", device_id=device_id, local=1)

    assert added["presets"] == [{"id": 0, "local": True}]
    info = added["preset"]
    assert info["local"] is True
    assert {"pan_pos", "tilt_pos", "zoom_pos", "wb_mode"} <= set(info["state"])
    # Where the camera points is recalled; the rest of the picture isn't,
    # unless the user says so
    pointing = ("pan_pos", "tilt_pos", "zoom_pos", "focus_pos", "focus_af_enabled")
    assert all(info["recall"][k] for k in ("pan_pos", "tilt_pos", "zoom_pos"))
    assert not any(v for k, v in info["recall"].items() if k not in pointing)


def test_adding_a_camera_preset_stores_it_on_the_camera(obs_world, tmp_path):
    device_id = obs_world.device_ids["visca-udp"]
    import_presets(obs_world, device_id, tmp_path, [])
    out = tmp_path / "add.json"
    obs_world.wait_for(lambda: run_and_read(obs_world, out, "get_preset", device_id=device_id, preset_id=0)
                       ["presets"] == [])

    added = run_and_read(obs_world, out, "add_preset", device_id=device_id, local=0)

    assert added["presets"] == [{"id": 0, "local": False}]
    assert "state" not in added["preset"]


def test_edit_dialog_changes_what_is_recalled_and_where_it_is_stored(obs_world, tmp_path):
    device_id = obs_world.device_ids["visca-udp"]
    import_presets(obs_world, device_id, tmp_path, [{
        "id": LOCAL_ID, "local": True,
        "state": {"pan_pos": 100, "tilt_pos": -200, "zoom_pos": 300},
        "recall": {"pan_pos": True, "tilt_pos": True, "zoom_pos": True},
    }])
    out = tmp_path / "edit.json"
    wait_for_preset(obs_world, device_id, LOCAL_ID, out, lambda p: p["preset"].get("local") is True)

    edited = run_and_read(obs_world, out, "edit_preset_dialog", device_id=device_id, preset_id=LOCAL_ID,
                          uncheck="tilt_pos,zoom_pos")
    assert edited["preset"]["recall"] == {"pan_pos": True, "tilt_pos": False, "zoom_pos": False}
    assert edited["preset"]["state"] == {"pan_pos": 100, "tilt_pos": -200, "zoom_pos": 300}

    edited = run_and_read(obs_world, out, "edit_preset_dialog", device_id=device_id, preset_id=LOCAL_ID,
                          storage="camera")
    assert edited["preset"]["local"] is False
    assert "state" not in edited["preset"]
    assert edited["presets"] == [{"id": LOCAL_ID, "local": False}]


def test_usb_presets_saved_before_local_presets_become_local_presets(obs_world, tmp_path):
    """A USB camera kept its presets' positions apart, in "presets_memory".
    They are its local presets' state now, and are saved that way."""
    device_id = obs_world.device_ids["usb-legacy-presets"]
    out = tmp_path / "preset.json"

    info = preset(obs_world, device_id, 2, out)
    assert info["presets"] == [{"id": 2, "local": True}, {"id": 5, "local": True}]
    assert info["preset"]["name"] == "Stage"
    assert info["preset"]["state"] == {"pan": 0.25, "tilt": -0.5, "zoom": 0.75, "focus": 0.5,
                                       "focus_af_enabled": False}
    assert all(info["preset"]["recall"].values())
    # A preset with no saved position has nothing to recall yet
    assert "state" not in preset(obs_world, device_id, 5, out)["preset"]

    saved = next(d for d in obs_world.saved_devices(tmp_path / "saved.json") if d.get("id") == device_id)
    assert "presets_memory" not in saved
    assert next(p for p in saved["presets"] if p["id"] == 2)["state"]["pan"] == 0.25
