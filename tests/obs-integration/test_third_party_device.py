"""Covers devices from a plugin other than this one: the plugin lists a
filter as a device, with nothing of the plugin's own to say so, when OBS says
it was added to a source and its proc_handler answers ptz_get_api_version
with a version it can use, and stops listing it once the filter is gone (see
"Providing a device" in docs/ptz-device-api.md).

tests/ui-harness/third-party-device-test.cpp registers two filters that stand
in for such a plugin: one at the plugin's own API version, and one a major
version later.
"""

import pytest
from obsws import ObsWebSocketError

GOOD_KIND = "ptz_test_third_party_device"
OTHER_VERSION_KIND = "ptz_test_third_party_device_other_version"


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
