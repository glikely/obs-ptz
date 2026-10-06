"""Covers the split between a device's *settings* (persisted, edited through
the properties tree PTZDevice::get_obs_properties() builds, which is what both
the plugin's own settings dialog and OBS's Filters dialog show) and its
transient *state* (whatever the camera reports, never saved).

Driven through tests/ui-harness/device-settings-test.cpp's
"get_device_settings" test (see World.device_settings() in conftest.py):

- every value the settings properties edit is a key the device also writes
  when saved -- otherwise it is either state that has no business in a
  settings tree (where OBS's Filters dialog would write it into the saved
  filter), or a setting that is silently lost;
- a PTZ filter persists only settings: no device id, name, or other runtime
  identity;
- settings sent through the plugin's own dialog path reach the filter's
  settings, and so the Filters dialog and the scene collection.
"""

import pytest

# The filter fixtures live in test_filter_devices.py
from test_filter_devices import camera_sim, cameras  # noqa: F401

IDENTITY_KEYS = {"id", "name"}

@pytest.mark.parametrize("backend", ["visca-tcp", "visca-udp", "visca-serial", "pelco-d", "pelco-p"])
def test_settings_properties_are_all_saved(obs_world, backend, tmp_path):
    keys = obs_world.device_settings(obs_world.device_names[backend], tmp_path / "settings.json")
    assert keys["property_keys"] - keys["save_keys"] == set()


def test_filter_settings_properties_are_all_saved(obs_world, cameras, tmp_path):  # noqa: F811
    cameras.add_source(obs_world.create_scene(), "settings-cam")
    cameras.add_filter("settings-cam")
    out = tmp_path / "settings.json"
    device_name = obs_world.wait_for_device_by_name("settings-cam", out, lambda r: r["found"] and r["bound"])["uuid"]

    keys = obs_world.device_settings(device_name, out)
    assert keys["property_keys"] - keys["save_keys"] == set()


def test_filter_persists_only_settings(obs_world, cameras, tmp_path):  # noqa: F811
    cameras.add_source(obs_world.create_scene(), "persist-cam")
    cameras.add_filter("persist-cam")
    out = tmp_path / "settings.json"
    device_name = obs_world.wait_for_device_by_name("persist-cam", out, lambda r: r["found"] and r["bound"])["uuid"]

    keys = obs_world.device_settings(device_name, out)
    assert keys["filter_keys"], "the device has no filter to persist"
    assert keys["filter_keys"] & IDENTITY_KEYS == set()
    assert {"type", "preset_max"} <= keys["filter_keys"]


def test_dialog_settings_reach_the_filter(obs_world, cameras, tmp_path):  # noqa: F811
    cameras.add_source(obs_world.create_scene(), "dialog-cam")
    cameras.add_filter("dialog-cam")
    out = tmp_path / "settings.json"
    device_name = obs_world.wait_for_device_by_name("dialog-cam", out, lambda r: r["found"] and r["bound"])["uuid"]

    # update_device seeds from the device's full save(), like the dialog does
    obs_world.run_ui_test("update_device", device=device_name, tcp_port=5679)

    def filter_settings():
        return obs_world.ws.call("GetSourceFilter", {"sourceName": "dialog-cam", "filterName": "PTZ"})["filterSettings"]

    obs_world.wait_for(lambda: filter_settings().get("tcp_port") == 5679)
    assert set(filter_settings()) & IDENTITY_KEYS == set()


def test_filter_updated_with_one_setting_keeps_the_rest_at_their_defaults(obs_world, cameras, tmp_path):  # noqa: F811
    cameras.add_source(obs_world.create_scene(), "partial-cam")
    cameras.add_filter("partial-cam")
    out = tmp_path / "settings.json"
    device_name = obs_world.wait_for_device_by_name("partial-cam", out, lambda r: r["found"] and r["bound"])["uuid"]

    # As OBS's Filters dialog or a script does: just the one setting, not the whole set
    obs_world.ws.call("SetSourceFilterSettings", {
        "sourceName": "partial-cam",
        "filterName": "PTZ",
        "filterSettings": {"pan_invert": True},
    })
    obs_world.wait_for(lambda: obs_world.device_settings(device_name, out)["saved"].get("pan_invert") is True)

    # nobody set these, and updating the one must not have lost their defaults
    saved = obs_world.device_settings(device_name, out)["saved"]
    assert saved["preset_max"] == 16
    assert saved["pantilt_speed_max"] == 1.0
