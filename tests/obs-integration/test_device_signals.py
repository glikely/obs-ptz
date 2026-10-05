"""Covers what PTZListModel tells a listener -- in practice the settings
dialog -- when a device changes: deviceStateUpdated(), carrying the state
values that changed, and deviceSettingsUpdated(), which has to fire however
the settings were changed, including by something that never goes near
PTZListModel::update().

Driven through tests/ui-harness/device-signals-test.cpp, which counts
both signals for each device once told to watch (see World.device_signals()
in conftest.py).
"""

import pytest

# The filter fixtures live in test_filter_devices.py
from test_filter_devices import camera_sim, cameras  # noqa: F401


def test_state_change_reports_what_changed(obs_world, tmp_path):
    device_name = obs_world.device_names["visca-tcp"]
    out = tmp_path / "signals.json"
    obs_world.wait_for_device_state(device_name, tmp_path / "state.json",
                                    lambda r: r["state"].get("wb_mode") is not None, timeout=10)
    obs_world.run_ui_test("watch_device_signals")

    obs_world.run_ui_test("set_device_state", device=device_name, wb_mode=1)
    obs_world.run_ui_test("set_device_state", device=device_name, wb_mode=2)

    # Nothing else changes wb_mode, so seeing it in a diff means the diff
    # got through with its values, and not empty
    signals = obs_world.wait_for_device_signals(device_name, out, lambda r: "wb_mode" in r["state_keys"])
    assert signals["state_count"] >= 1


def test_settings_update_is_announced(obs_world, tmp_path):
    device_name = obs_world.device_names["visca-tcp"]
    out = tmp_path / "signals.json"
    obs_world.run_ui_test("watch_device_signals")

    # a whole, unedited settings update, as the dialog's Apply makes
    obs_world.run_ui_test("update_device", device=device_name)

    obs_world.wait_for_device_signals(device_name, out, lambda r: r["settings_count"] >= 1)


def test_settings_edited_behind_the_models_back_are_announced(obs_world, cameras, tmp_path):  # noqa: F811
    cameras.add_source(obs_world.create_scene(), "signals-cam")
    cameras.add_filter("signals-cam")
    out = tmp_path / "signals.json"
    device_name = obs_world.wait_for_device_by_name("signals-cam", out, lambda r: r["found"] and r["bound"])["uuid"]
    obs_world.run_ui_test("watch_device_signals")

    # obs-websocket edits the filter's settings the way OBS's Filters dialog
    # does: straight to the source, not through the plugin
    obs_world.ws.call("SetSourceFilterSettings", {
        "sourceName": "signals-cam",
        "filterName": "PTZ",
        "filterSettings": {"preset_max": 8},
    })

    obs_world.wait_for_device_signals(device_name, out, lambda r: r["settings_count"] >= 1)
