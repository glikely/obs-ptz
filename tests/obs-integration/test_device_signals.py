"""Covers what PTZListModel tells a listener -- in practice the settings
dialog -- when a device changes: that the state that changed reaches it, with
its values, and that a settings update does, however the settings were changed,
including by something that never goes near PTZListModel::update().

Seen from the listener, as a user sees it: the dialog (see
World.open_settings_dialog() and World.settings_dialog() in conftest.py) shows a
changed state in its status view, and redraws its settings view for a changed
setting.
"""

import pytest

# The filter fixtures live in test_filter_devices.py
from test_filter_devices import camera_sim, cameras  # noqa: F401


def test_state_change_reports_what_changed(obs_world, tmp_path):
    device_name = obs_world.device_names["visca-tcp"]
    out = tmp_path / "dialog.json"
    obs_world.wait_for_device_state(device_name, tmp_path / "state.json",
                                    lambda r: r["state"].get("wb_mode") is not None, timeout=10)
    obs_world.open_settings_dialog(device_name)

    obs_world.set_device_state(device_name, wb_mode=1)
    obs_world.set_device_state(device_name, wb_mode=2)

    # Nothing else changes wb_mode, so seeing its value shown means the change
    # got through with its values, and not empty
    dialog = obs_world.wait_for_settings_dialog(out, lambda r: r["wb_mode"] == 2)
    assert dialog["state_updates"] >= 1


def test_settings_update_is_announced(obs_world, tmp_path):
    device_name = obs_world.device_names["visca-tcp"]
    out = tmp_path / "dialog.json"
    obs_world.open_settings_dialog(device_name)

    # a whole, unedited settings update, as the dialog's Apply makes
    obs_world.update_device(device_name)

    obs_world.wait_for_settings_dialog(out, lambda r: r["settings_refreshes"] >= 1)


def test_settings_edited_behind_the_models_back_are_announced(obs_world, cameras, tmp_path):  # noqa: F811
    cameras.add_source(obs_world.create_scene(), "signals-cam")
    cameras.add_filter("signals-cam")
    out = tmp_path / "dialog.json"
    device_name = obs_world.wait_for_device_by_name("signals-cam", out, lambda r: r["found"] and r["bound"])["uuid"]
    obs_world.open_settings_dialog(device_name)

    # obs-websocket edits the filter's settings the way OBS's Filters dialog
    # does: straight to the source, not through the plugin
    obs_world.ws.call("SetSourceFilterSettings", {
        "sourceName": "signals-cam",
        "filterName": "PTZ",
        "filterSettings": {"preset_max": 8},
    })

    obs_world.wait_for_settings_dialog(out, lambda r: r["settings_refreshes"] >= 1)
