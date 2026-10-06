"""Covers the settings of a device's source being the one copy of what the
device saves: what the device changes itself, its presets, is written to them
as it changes, rather than only when OBS saves, so reading them is reading the
device, and an update from them does not undo it.

Before that, the filter's settings only had the presets the device had last
been saved with, and any update, from the Filters dialog or obs-websocket,
brought the device back to those.
"""

from test_filter_devices import camera_sim, cameras  # noqa: F401


def filter_settings(world, source):
    return world.ws.call("GetSourceFilter", {"sourceName": source, "filterName": "PTZ"})["filterSettings"]


def test_a_preset_the_device_makes_is_in_the_settings_at_once(obs_world, cameras, tmp_path):  # noqa: F811
    cameras.add_source(obs_world.create_scene(), "persist-preset-cam")
    cameras.add_filter("persist-preset-cam")
    out = tmp_path / "device.json"
    uuid = obs_world.wait_for_device_by_name("persist-preset-cam", out, lambda r: r["found"] and r["bound"])["uuid"]
    assert filter_settings(obs_world, "persist-preset-cam").get("presets", []) == []

    obs_world.run_ui_test("add_preset", device=uuid)

    obs_world.wait_for(lambda: len(filter_settings(obs_world, "persist-preset-cam").get("presets", [])) == 1)


def test_an_update_of_another_setting_keeps_the_presets(obs_world, cameras, tmp_path):  # noqa: F811
    cameras.add_source(obs_world.create_scene(), "persist-update-cam")
    cameras.add_filter("persist-update-cam")
    out = tmp_path / "device.json"
    uuid = obs_world.wait_for_device_by_name("persist-update-cam", out, lambda r: r["found"] and r["bound"])["uuid"]
    obs_world.run_ui_test("add_preset", device=uuid)
    obs_world.wait_for(lambda: len(filter_settings(obs_world, "persist-update-cam").get("presets", [])) == 1)

    obs_world.ws.call("SetSourceFilterSettings", {
        "sourceName": "persist-update-cam",
        "filterName": "PTZ",
        "filterSettings": {"pan_invert": True},
    })

    settings_out = tmp_path / "settings.json"
    obs_world.wait_for(lambda: obs_world.device_settings(uuid, settings_out)["saved"].get("pan_invert") is True)
    assert len(obs_world.device_settings(uuid, settings_out)["saved"]["presets"]) == 1
