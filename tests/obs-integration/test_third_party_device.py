"""Covers devices from a plugin other than this one: the plugin lists a
filter, or a source, as a device, with nothing of the plugin's own to say so,
when OBS says it was added to a source or created and its proc_handler answers
ptz_get_api_version with a version it can use, and stops listing it once it
is gone (see "Providing a device" in docs/ptz-device-api.md).

tests/ui-harness/third-party-device-test.cpp registers filters and a source
that stand in for such a plugin: a filter and a source at the plugin's own API
version, and a filter a major version later.
"""

import pytest
from obsws import ObsWebSocketError

GOOD_KIND = "ptz_test_third_party_device"
OTHER_VERSION_KIND = "ptz_test_third_party_device_other_version"
SOURCE_KIND = "ptz_test_third_party_source_device"


@pytest.fixture
def source(obs_world):
    """A source for a filter to be added to, removed at the end"""
    name = "third-party-cam"
    obs_world.ws.call("CreateInput", {
        "sceneName": obs_world.create_scene(),
        "inputName": name,
        "inputKind": "color_source_v3",
        "inputSettings": {},
    })
    yield name
    try:
        obs_world.ws.call("RemoveInput", {"inputName": name})
    except ObsWebSocketError:
        pass


def add_filter(world, source, kind):
    world.ws.call("CreateSourceFilter", {
        "sourceName": source,
        "filterName": "Third party",
        "filterKind": kind,
        "filterSettings": {},
    })


def test_a_filter_at_a_usable_version_is_a_device(obs_world, source, tmp_path):
    add_filter(obs_world, source, GOOD_KIND)

    found = obs_world.wait_for_device_by_name(source, tmp_path / "device.json", lambda r: r["found"])
    assert found["name"] == source
    assert found["uuid"]


def test_removing_the_filter_removes_the_device(obs_world, source, tmp_path):
    out = tmp_path / "device.json"
    add_filter(obs_world, source, GOOD_KIND)
    obs_world.wait_for_device_by_name(source, out, lambda r: r["found"])

    obs_world.ws.call("RemoveSourceFilter", {"sourceName": source, "filterName": "Third party"})

    obs_world.wait_for_device_by_name(source, out, lambda r: not r["found"], timeout=10)


def test_a_filter_at_another_major_version_is_not(obs_world, source, tmp_path):
    out = tmp_path / "device.json"
    add_filter(obs_world, source, OTHER_VERSION_KIND)
    # A device that is going to be listed is by the time a plugin's own is
    good = "third-party-cam-good"
    obs_world.ws.call("CreateInput", {
        "sceneName": obs_world.create_scene(),
        "inputName": good,
        "inputKind": "color_source_v3",
        "inputSettings": {},
    })
    try:
        add_filter(obs_world, good, GOOD_KIND)
        obs_world.wait_for_device_by_name(good, out, lambda r: r["found"])

        assert obs_world.device_by_name(source, out)["found"] is False
    finally:
        obs_world.ws.call("RemoveInput", {"inputName": good})


def test_a_source_at_a_usable_version_is_a_device(obs_world, tmp_path):
    out = tmp_path / "device.json"
    name = "third-party-source-device"
    scene = obs_world.create_scene()
    item = obs_world.ws.call("CreateInput", {
        "sceneName": scene,
        "inputName": name,
        "inputKind": SOURCE_KIND,
        "inputSettings": {},
    })["sceneItemId"]
    try:
        found = obs_world.wait_for_device_by_name(name, out, lambda r: r["found"])
        assert found["name"] == name
        assert found["uuid"]
    finally:
        # The scene item holds the source, and a scene that is not showing is not drawn
        # to let go of it, so take the item out first; that can be the end of the source
        obs_world.ws.call("RemoveSceneItem", {"sceneName": scene, "sceneItemId": item})
        try:
            obs_world.ws.call("RemoveInput", {"inputName": name})
        except ObsWebSocketError:
            pass

    obs_world.wait_for_device_by_name(name, out, lambda r: not r["found"], timeout=10)
