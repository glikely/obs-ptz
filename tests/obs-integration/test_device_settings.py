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
from test_filter_devices import FILTER_KIND, camera_sim, cameras  # noqa: F401

IDENTITY_KEYS = {"id", "name"}

@pytest.mark.parametrize("backend", ["visca-tcp", "visca-udp", "visca-serial", "pelco-d", "pelco-p"])
def test_settings_properties_are_all_saved(obs_world, backend, tmp_path):
    keys = obs_world.device_settings(obs_world.device_names[backend], tmp_path / "settings.json")
    assert keys["property_keys"] - keys["save_keys"] == set()


@pytest.mark.parametrize("backend,has_tally", [("visca-tcp", True), ("pelco-d", False)])
def test_tally_auto_is_offered_for_a_camera_with_tally_lamps(obs_world, backend, has_tally, tmp_path):
    """The setting is saved, and has its default, for every device, but there
    is only something to set for one with the tally_light feature"""
    keys = obs_world.device_settings(obs_world.device_names[backend], tmp_path / "settings.json")
    assert "tally_auto" in keys["save_keys"]
    assert ("tally_auto" in keys["property_keys"]) is has_tally


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
    # Only what was set is saved: a setting at its default is not, and gets it
    # again when the filter is made
    assert "type" in keys["filter_keys"]


def test_dialog_settings_reach_the_filter(obs_world, cameras, tmp_path):  # noqa: F811
    cameras.add_source(obs_world.create_scene(), "dialog-cam")
    cameras.add_filter("dialog-cam")
    out = tmp_path / "settings.json"
    device_name = obs_world.wait_for_device_by_name("dialog-cam", out, lambda r: r["found"] and r["bound"])["uuid"]

    # update_device seeds from the device's full save(), like the dialog does
    obs_world.update_device(device_name, tcp_port=5679)

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
    assert saved["pan_speed_max"] == 1.0
    assert saved["tilt_speed_max"] == 1.0


@pytest.mark.parametrize("model,has_camera_store", [("0123:0001", True), ("0123:0008", False)])
def test_max_presets_is_offered_for_a_camera_that_keeps_presets(request, obs_world, tmp_path, model,
                                                                has_camera_store):
    """preset_max is how many numbered slots to use of a camera that can't
    say how many it has, which a VISCA device with only the local store has
    no use for: it is saved and has its default, but only offered for one
    with the camera store. The settings dialog shows the properties offered. (Model 0123:0008 is
    test_visca_user_profiles.py's, whose command set has no memory_recall.)"""
    from test_visca_user_profiles import Camera
    camera = Camera(request, obs_world, tmp_path, model)
    camera.wait_for_state(lambda s: "pan" in s)
    keys = obs_world.device_settings(camera.device_name, tmp_path / "settings.json")
    assert "preset_max" in keys["save_keys"]
    assert ("preset_max" in keys["property_keys"]) is has_camera_store


@pytest.mark.parametrize("given,pan,tilt", [
    ({"pantilt_speed_max": 0.5}, 0.5, 0.5),
    ({"pantilt_speed_max": 0.5, "tilt_speed_max": 0.25}, 0.5, 0.25),
])
def test_an_older_pantilt_speed_max_becomes_pan_and_tilt(obs_world, cameras, tmp_path, given, pan,  # noqa: F811
                                                          tilt):
    """Older versions saved one cap for pan and tilt, pantilt_speed_max,
    which a filter made with it takes as each's, but not over one of
    them set since; and which it then saves as them, not as itself."""
    cameras.add_source(obs_world.create_scene(), "old-speed-cam")
    obs_world.ws.call("CreateSourceFilter", {
        "sourceName": "old-speed-cam",
        "filterName": "PTZ",
        "filterKind": FILTER_KIND,
        "filterSettings": {
            "type": "visca-over-tcp",
            "host": "127.0.0.1",
            "tcp_port": cameras.sim.tcp_port,
            **given,
        },
    })
    out = tmp_path / "settings.json"
    device_name = obs_world.wait_for_device_by_name("old-speed-cam", out, lambda r: r["found"] and r["bound"])["uuid"]

    saved = obs_world.device_settings(device_name, out)["saved"]
    assert (saved["pan_speed_max"], saved["tilt_speed_max"]) == (pan, tilt)
    settings = obs_world.ws.call("GetSourceFilter", {"sourceName": "old-speed-cam", "filterName": "PTZ"})["filterSettings"]
    assert "pantilt_speed_max" not in settings
    assert (settings["pan_speed_max"], settings["tilt_speed_max"]) == (pan, tilt)
